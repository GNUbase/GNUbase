// SPDX-License-Identifier: GPL-3.0-or-later
// The key editor in the engine (#127, docs/features/key-editor.md): the selected notes in the editor views, step and
// MIDI input through editor.input, and chord editing with note.chord and note.voicing. tests/keyeditor_ui_tests.cpp
// drives the key editor in the app.
#include "engine/engine.h"
#include "engine/midi/chords.h"
#include "engine/time/tempomap.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;
	constexpr int64_t g_sixteenth = g_quarter / 4;

	std::string chordName(const std::vector<int>& pitches)
	{
		const std::optional<engine::midi::Chord> chord = engine::midi::recognizeChord(pitches);
		return chord ? chord->name() : "none";
	}

	struct Fixture
	{
		Fixture()
		{
			track = call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
			part = call("midiPart.add", {{"track", track}, {"position", 4 * g_quarter}, {"length", 4 * g_quarter}})
					   .at("part")
					   .at("id")
					   .get<uint64_t>();
			call("editor.open", {{"editor", "window"}, {"parts", Json::array({part})}});
		}

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

		// Adds notes {position, pitch} a sixteenth long; returns their ids.
		std::vector<uint64_t> notes(const std::vector<std::pair<int64_t, int>>& notes)
		{
			Json items = Json::array();
			for (const auto& [position, pitch] : notes)
				items.push_back({{"position", position}, {"length", g_sixteenth}, {"pitch", pitch}, {"velocity", 0.5}});
			std::vector<uint64_t> ids;
			const Json added = call("note.add", {{"part", part}, {"notes", items}});
			for (const Json& note : added.at("notes"))
				ids.push_back(note.at("id").get<uint64_t>());
			return ids;
		}

		// The part's notes by position, then pitch.
		Json partNotes()
		{
			const Json project = call("project.get");
			for (const Json& each : project.at("midiParts"))
				if (each.at("id") == part)
				{
					std::vector<Json> sorted(each.at("notes").begin(), each.at("notes").end());
					std::sort(sorted.begin(), sorted.end(),
						[](const Json& a, const Json& b)
						{
							return std::make_pair(a.at("position").get<int64_t>(), a.at("pitch").get<int>()) <
								std::make_pair(b.at("position").get<int64_t>(), b.at("pitch").get<int>());
						});
					return sorted;
				}
			return Json::array();
		}

		Json view() { return call("editor.get", {{"editor", "window"}}); }

		Json selection(const std::vector<uint64_t>& notes)
		{
			Json items = Json::array();
			for (const uint64_t note : notes)
				items.push_back({{"part", part}, {"note", note}});
			return items;
		}

		std::vector<int> pitches(const std::vector<uint64_t>& ids)
		{
			std::vector<int> found;
			const Json notes = partNotes();
			for (const uint64_t id : ids)
				for (const Json& note : notes)
					if (note.at("id") == id)
						found.push_back(note.at("pitch").get<int>());
			return found;
		}

		engine::Engine engine;
		engine::Client client{engine};
		uint64_t track = 0;
		uint64_t part = 0;
	};
}

TEST_CASE("the key editor's selected notes are view state of the editable parts", "[unit]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.notes({{0, 60}, {g_quarter, 62}});
	const int64_t revision = f.call("project.revision").at("revision").get<int64_t>();
	f.call("editor.set", {{"editor", "window"}, {"selection", f.selection({ids[1], ids[0], ids[1]})}});
	CHECK(f.view().at("selection") == f.selection({ids[1], ids[0]}));
	CHECK(f.call("project.revision").at("revision").get<int64_t>() == revision); // no undo step
	CHECK_THAT(f.fails("editor.set",
				   {{"editor", "window"}, {"selection", Json::array({{{"part", f.part}, {"note", 999999}}})}}),
		ContainsSubstring("has no note"));
	CHECK_THAT(f.fails("editor.set", {{"editor", "lower"}, {"selection", f.selection({ids[0]})}}),
		ContainsSubstring("isn't editable"));
	// A removed note leaves the selection, and closing the editor clears it.
	f.call("note.remove", {{"part", f.part}, {"notes", Json::array({ids[0]})}});
	CHECK(f.view().at("selection") == f.selection({ids[1]}));
	f.call("editor.close", {{"editor", "window"}});
	CHECK(f.view().at("selection").empty());
}

TEST_CASE("step input inserts notes and chords at the step input cursor and moves it on", "[unit]")
{
	Fixture f;
	CHECK_THAT(f.fails("editor.input", {{"editor", "window"}, {"notes", Json::array({{{"pitch", 60}}})}}),
		ContainsSubstring("step input and MIDI input are off"));
	// Turning step input on puts the cursor at the active part's start (bar 2).
	f.call("editor.set", {{"editor", "window"}, {"stepInput", true}, {"lengthQuantize", "1/8"}});
	CHECK(f.view().at("stepPosition") == 4 * g_quarter);
	f.call("editor.input", {{"editor", "window"}, {"notes", Json::array({{{"pitch", 64}, {"velocity", 0.25}}})}});
	// A chord, then a rest (the cursor moved by hand), then a note.
	const Json chord = f.call("editor.input",
		{{"editor", "window"},
			{"notes", Json::array({{{"pitch", 67}, {"velocity", 1.0}}, {{"pitch", 60}, {"velocity", 1.0}}})}});
	CHECK(chord.at("part") == f.part);
	CHECK(chord.at("notes").size() == 2);
	f.call("editor.set", {{"editor", "window"}, {"stepPosition", (4 * g_quarter) + (3 * g_sixteenth)}});
	f.call("editor.input", {{"editor", "window"}, {"notes", Json::array({{{"pitch", 72}}})}});
	const Json notes = f.partNotes();
	REQUIRE(notes.size() == 4);
	CHECK(notes[0].at("position") == 0);
	CHECK(notes[0].at("length") == 2 * g_sixteenth); // the length quantize's 1/8
	CHECK(notes[0].at("pitch") == 64);
	CHECK(notes[0].at("velocity").get<double>() == Approx(0.25).margin(1e-4));
	CHECK(notes[1].at("position") == g_sixteenth); // a step of the quantize grid (1/16) later
	CHECK(notes[1].at("pitch") == 60);
	CHECK(notes[2].at("pitch") == 67);
	CHECK(notes[3].at("position") == 3 * g_sixteenth);
	CHECK(notes[3].at("velocity").get<double>() == Approx(100.0 / 127.0).margin(1e-4)); // insertVelocity
	CHECK(f.view().at("stepPosition") == (4 * g_quarter) + (4 * g_sixteenth));
	CHECK(f.view().at("selection").size() == 1); // the last note inserted
	// One undo step each.
	f.call("history.undo");
	CHECK(f.partNotes().size() == 3);

	SECTION("without Keep Existing Notes a note replaces the ones starting within the step; with it they stay")
	{
		f.call("editor.set", {{"editor", "window"}, {"stepPosition", 4 * g_quarter}});
		f.call("editor.input", {{"editor", "window"}, {"notes", Json::array({{{"pitch", 50}}})}});
		CHECK(f.partNotes().size() == 3);
		CHECK(f.partNotes()[0].at("pitch") == 50);
		f.call("editor.set", {{"editor", "window"}, {"stepPosition", 4 * g_quarter}, {"keepNotes", true}});
		f.call("editor.input", {{"editor", "window"}, {"notes", Json::array({{{"pitch", 52}}})}});
		CHECK(f.partNotes().size() == 4);
	}
	SECTION("Move Insert Mode moves the notes from the cursor on by a step")
	{
		f.call("editor.set", {{"editor", "window"}, {"stepPosition", 4 * g_quarter}, {"moveInsert", true}});
		f.call("editor.input", {{"editor", "window"}, {"notes", Json::array({{{"pitch", 50}}})}});
		const Json moved = f.partNotes();
		REQUIRE(moved.size() == 4);
		CHECK(moved[0].at("pitch") == 50);
		CHECK(moved[1].at("position") == g_sixteenth);
		CHECK(moved[1].at("pitch") == 64);
		CHECK(moved[2].at("position") == 2 * g_sixteenth);
	}
	SECTION("the cursor must be within the active part")
	{
		f.call("editor.set", {{"editor", "window"}, {"stepPosition", 0}});
		CHECK_THAT(f.fails("editor.input", {{"editor", "window"}, {"notes", Json::array({{{"pitch", 60}}})}}),
			ContainsSubstring("isn't within the active part"));
	}
}

TEST_CASE("MIDI input changes the selected note and selects the next one", "[unit]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.notes({{0, 60}, {g_sixteenth, 62}, {2 * g_sixteenth, 64}});
	f.call("editor.set", {{"editor", "window"}, {"midiInput", true}, {"selection", f.selection({ids[0]})}});
	f.call("editor.input",
		{{"editor", "window"}, {"notes", Json::array({{{"pitch", 48}, {"velocity", 1.0}, {"releaseVelocity", 0.0}}})}});
	Json notes = f.partNotes();
	REQUIRE(notes.size() == 3);
	CHECK(notes[0].at("id") == ids[0]);
	CHECK(notes[0].at("pitch") == 48);
	CHECK(notes[0].at("velocity") == 1.0);
	CHECK(notes[0].at("releaseVelocity").get<double>() == Approx(0.5).margin(1e-4)); // not recorded
	CHECK(f.view().at("selection") == f.selection({ids[1]}));

	// Without Record Pitch the pitch stays; a chord adds notes with the selected note's position and length.
	f.call("editor.set", {{"editor", "window"}, {"recordPitch", false}});
	f.call("editor.input",
		{{"editor", "window"},
			{"notes", Json::array({{{"pitch", 70}, {"velocity", 0.25}}, {{"pitch", 67}, {"velocity", 0.25}}})}});
	notes = f.partNotes();
	REQUIRE(notes.size() == 4);
	CHECK(notes[1].at("pitch") == 62);
	CHECK(notes[1].at("velocity").get<double>() == Approx(0.25).margin(1e-4));
	CHECK(notes[2].at("position") == g_sixteenth);
	CHECK(notes[2].at("pitch") == 70);
	CHECK(f.view().at("selection") == f.selection({ids[2]}));

	// With Keep Existing Notes the selected note stays and the played ones are added.
	f.call("editor.set", {{"editor", "window"}, {"keepNotes", true}});
	f.call("editor.input", {{"editor", "window"}, {"notes", Json::array({{{"pitch", 76}}})}});
	notes = f.partNotes();
	REQUIRE(notes.size() == 5);
	CHECK(notes[3].at("pitch") == 64);
	CHECK(notes[4].at("pitch") == 76);
	CHECK(notes[4].at("position") == 2 * g_sixteenth);

	f.call("editor.set", {{"editor", "window"}, {"selection", Json::array()}});
	CHECK_THAT(f.fails("editor.input", {{"editor", "window"}, {"notes", Json::array({{{"pitch", 60}}})}}),
		ContainsSubstring("needs a selected note"));
}

TEST_CASE("chords are recognized and revoiced", "[unit]")
{
	using engine::midi::recognizeChord;
	CHECK(chordName({60, 64, 67}) == "C");
	CHECK(chordName({64, 67, 72}) == "C"); // first inversion
	CHECK(chordName({57, 60, 64, 67}) == "Am7");
	CHECK(chordName({62, 65, 69, 72, 74}) == "Dm7"); // octaves don't count
	CHECK(chordName({66, 70, 73}) == "F#");
	CHECK(chordName({59, 62, 65}) == "Bdim");
	CHECK(!recognizeChord({60, 61, 62}).has_value());
	CHECK(!recognizeChord({}).has_value());

	using engine::midi::fitChord;
	const auto* minor = engine::midi::chordType("minor");
	REQUIRE(minor != nullptr);
	CHECK(fitChord({60, 64, 67}, *minor) == std::vector<int>{60, 63, 67});
	CHECK(fitChord({67, 60, 64}, *minor) == std::vector<int>{67, 60, 63}); // in the order given
	CHECK(fitChord({60, 64, 67, 72}, *engine::midi::chordType("major7")) == std::vector<int>{60, 64, 67, 71});
	CHECK(fitChord({60, 62, 64}, *engine::midi::chordType("sus4")) == std::vector<int>{60, 65, 67});

	using engine::midi::revoice;
	using engine::midi::Voicing;
	CHECK(*revoice({60, 64, 67}, Voicing::InvertUp) == std::vector<int>{72, 64, 67});
	CHECK(*revoice({60, 64, 67}, Voicing::InvertDown) == std::vector<int>{60, 64, 55});
	CHECK(*revoice({60, 64, 67, 71}, Voicing::Drop2) == std::vector<int>{60, 64, 55, 71});
	CHECK(*revoice({60, 64, 67, 71}, Voicing::Drop3) == std::vector<int>{60, 52, 67, 71});
	CHECK(*revoice({60, 64, 67, 71}, Voicing::Drop24) == std::vector<int>{48, 64, 55, 71});
	CHECK(!revoice({60, 64, 67}, Voicing::Drop24).has_value());
	CHECK(!revoice({60}, Voicing::InvertUp).has_value());
	CHECK(!revoice({120, 124, 127}, Voicing::InvertUp).has_value());
	CHECK(engine::midi::pitchName(60) == "C3");
}

TEST_CASE("note.chord and note.voicing edit the chord of notes", "[unit]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.notes({{0, 60}, {0, 64}, {g_sixteenth, 67}}); // arpeggiated
	const Json chord = f.call("note.chord", {{"part", f.part}, {"notes", ids}}).at("chord");
	CHECK(chord.at("name") == "C");
	CHECK(chord.at("root") == 0);
	CHECK(chord.at("type") == "major");
	CHECK(f.call("note.chord", {{"part", f.part}, {"notes", {ids[0], ids[2]}}}).at("chord").is_null());

	const Json changed = f.call("note.voicing", {{"part", f.part}, {"notes", ids}, {"chord", "minor"}});
	CHECK(changed.at("notes").size() == 1);
	CHECK(f.pitches(ids) == std::vector<int>{60, 63, 67});
	f.call("note.voicing", {{"part", f.part}, {"notes", ids}, {"voicing", "invertUp"}});
	CHECK(f.pitches(ids) == std::vector<int>{72, 63, 67});
	f.call("history.undo");
	CHECK(f.pitches(ids) == std::vector<int>{60, 63, 67});
	CHECK_THAT(f.fails("note.voicing", {{"part", f.part}, {"notes", ids}, {"voicing", "drop24"}}),
		ContainsSubstring("can't take that voicing"));
	CHECK_THAT(
		f.fails("note.voicing", {{"part", f.part}, {"notes", ids}}), ContainsSubstring("either chord or voicing"));
}
