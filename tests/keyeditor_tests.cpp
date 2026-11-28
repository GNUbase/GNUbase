// SPDX-License-Identifier: GPL-3.0-or-later
// The key editor in the engine (#127, docs/features/key-editor.md): the selected notes in the editor views, step and
// MIDI input through editor.input, and chord editing with note.chord and note.voicing. tests/keyeditor_ui_tests.cpp
// drives the key editor in the app.
#include "engine/engine.h"
#include "engine/midi/chords.h"
#include "engine/time/tempomap.h"
#include "platform/audio/device.h"
#include "platform/hosttime.h"
#include "platform/midi/dummyport.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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

	// Only the dummy MIDI backend, so that step input, which listens to the MIDI inputs, opens no real ones.
	std::vector<std::unique_ptr<midi::Backend>> dummyMidi(midi::DummyBackend** backend = nullptr)
	{
		auto dummy = std::make_unique<midi::DummyBackend>();
		if (backend != nullptr)
			*backend = dummy.get();
		std::vector<std::unique_ptr<midi::Backend>> backends;
		backends.push_back(std::move(dummy));
		return backends;
	}

	struct Fixture
	{
		explicit Fixture(std::vector<std::unique_ptr<midi::Backend>> midiBackends = dummyMidi())
			: engine(audio::makeBackends(), {}, std::move(midiBackends))
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

TEST_CASE("the selection keeps only notes of the parts the editing mode makes editable", "[unit]")
{
	Fixture f;
	const uint64_t other =
		f.call("midiPart.add", {{"track", f.track}, {"position", 8 * g_quarter}, {"length", 4 * g_quarter}})
			.at("part")
			.at("id")
			.get<uint64_t>();
	f.call("editor.open", {{"editor", "window"}, {"parts", Json::array({f.part, other})}});
	const uint64_t mine = f.notes({{0, 60}}).front();
	const uint64_t theirs =
		f.call("note.add",
			 {{"part", other},
				 {"notes",
					 Json::array({{{"position", 0}, {"length", g_sixteenth}, {"pitch", 62}, {"velocity", 0.5}}})}})
			.at("notes")
			.at(0)
			.at("id")
			.get<uint64_t>();
	const Json both = Json::array({{{"part", f.part}, {"note", mine}}, {{"part", other}, {"note", theirs}}});
	f.call("editor.set", {{"editor", "window"}, {"selection", both}});
	CHECK(f.view().at("selection") == both);
	// Active Part mode (p. 1197): editor.changed and editor.get keep only the active part's notes.
	const Json changed = f.call("editor.set", {{"editor", "window"}, {"editingMode", "activePart"}});
	CHECK(changed.at("selection") == f.selection({mine}));
	CHECK(f.view().at("selection") == f.selection({mine}));
	// Another active part in that mode drops them as well.
	f.call("editor.set", {{"editor", "window"}, {"activePart", other}});
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
		f.call("editor.set",
			{{"editor", "window"}, {"stepPosition", 4 * g_quarter}, {"moveInsert", true}, {"lengthQuantize", "1/16"}});
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

TEST_CASE(
	"step input takes the channel of a note that stays, makes room for long notes and ends them at the part's end",
	"[unit]")
{
	Fixture f;
	const std::vector<uint64_t> ids = f.notes({{0, 60}, {g_sixteenth, 61}, {2 * g_sixteenth, 62}});
	f.call("note.set",
		{{"part", f.part},
			{"notes",
				Json::array({{{"id", ids[0]}, {"channel", 3}}, {{"id", ids[1]}, {"channel", 5}},
					{{"id", ids[2]}, {"channel", 9}}})}});
	f.call("editor.set", {{"editor", "window"}, {"stepInput", true}, {"lengthQuantize", "1/4"}});
	const auto input = [&f](int pitch)
	{ return f.call("editor.input", {{"editor", "window"}, {"notes", Json::array({{{"pitch", pitch}}})}}); };

	SECTION("a replaced note gives no channel: the last one before the cursor that stays does")
	{
		f.call("editor.set", {{"editor", "window"}, {"stepPosition", (4 * g_quarter) + (2 * g_sixteenth)}});
		const Json added = input(64).at("notes");
		REQUIRE(added.size() == 1);
		CHECK(added[0].at("channel") == 5);
	}
	SECTION("at the part's start, the first note that stays")
	{
		f.call("editor.set", {{"editor", "window"}, {"stepPosition", 4 * g_quarter}});
		CHECK(input(64).at("notes")[0].at("channel") == 5);
	}
	SECTION("Move Insert Mode moves the notes by the inserted notes' length where it is longer than a step")
	{
		f.call("editor.set", {{"editor", "window"}, {"stepPosition", 4 * g_quarter}, {"moveInsert", true}});
		input(64);
		const Json notes = f.partNotes();
		REQUIRE(notes.size() == 4);
		CHECK(notes[0].at("pitch") == 64);
		CHECK(notes[0].at("length") == g_quarter);
		CHECK(notes[1].at("position") == g_quarter);
		CHECK(notes[2].at("position") == g_quarter + g_sixteenth);
		CHECK(notes[3].at("position") == g_quarter + (2 * g_sixteenth));
		CHECK(f.view().at("stepPosition") == (4 * g_quarter) + g_sixteenth); // the cursor still moves by a step
	}
	SECTION("an inserted note ends at the part's end")
	{
		f.call("editor.set", {{"editor", "window"}, {"stepPosition", (8 * g_quarter) - g_sixteenth}});
		CHECK(input(64).at("notes")[0].at("length") == g_sixteenth);
	}
	CHECK(f.call("history.get").at("steps").back().at("action") == "Note input");
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

	// A played pitch above the selected note's: the next note is selected, not the changed one (p. 1219).
	f.call("editor.set",
		{{"editor", "window"}, {"keepNotes", false}, {"recordPitch", true}, {"selection", f.selection({ids[0]})}});
	f.call("editor.input", {{"editor", "window"}, {"notes", Json::array({{{"pitch", 72}}})}});
	CHECK(f.pitches({ids[0]}) == std::vector<int>{72});
	CHECK(f.view().at("selection") == f.selection({ids[1]}));

	CHECK(f.call("history.get").at("steps").back().at("action") == "Note input"); // not step input's

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
	CHECK(revoice({60, 64, 67}, Voicing::InvertUp) == std::vector<int>{72, 64, 67});
	CHECK(revoice({60, 64, 67}, Voicing::InvertDown) == std::vector<int>{60, 64, 55});
	CHECK(revoice({60, 64, 67, 71}, Voicing::Drop2) == std::vector<int>{60, 64, 55, 71});
	CHECK(revoice({60, 64, 67, 71}, Voicing::Drop3) == std::vector<int>{60, 52, 67, 71});
	CHECK(revoice({60, 64, 67, 71}, Voicing::Drop24) == std::vector<int>{48, 64, 55, 71});
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

TEST_CASE("the key editor's Acoustic Feedback is view state and off at first", "[unit]")
{
	// Acoustic Feedback in the toolbar (reference manual p. 1192).
	Fixture f;
	CHECK(f.view().at("acousticFeedback") == false);
	const int64_t revision = f.call("project.revision").at("revision").get<int64_t>();
	CHECK(f.call("editor.set", {{"editor", "window"}, {"acousticFeedback", true}}).at("acousticFeedback") == true);
	CHECK(f.view().at("acousticFeedback") == true);
	CHECK(f.call("project.revision").at("revision").get<int64_t>() == revision); // no undo step
	CHECK(!f.fails("editor.set", {{"editor", "window"}, {"acousticFeedback", "on"}}).empty());
}

TEST_CASE("Indicate Transpositions gives the transposition the parts' notes play with", "[unit]")
{
	// Indicate Transpositions (#1766, reference manual pp. 419, 1260): off at first, without transpositions.
	Fixture f;
	CHECK(f.view().at("indicateTranspositions") == false);
	CHECK(f.view().at("transpositions") == Json::array());
	const int64_t revision = f.call("project.revision").at("revision").get<int64_t>();
	const auto steps = [&f]
	{
		const Json shown = f.call("editor.set", {{"editor", "window"}, {"indicateTranspositions", true}});
		REQUIRE(shown.at("transpositions").size() == 1);
		CHECK(shown.at("transpositions").at(0).at("part") == f.part);
		return shown.at("transpositions").at(0).at("steps");
	};
	const auto step = [](int64_t position, int semitones)
	{ return Json{{"position", position}, {"semitones", semitones}}; };
	CHECK(steps() == Json::array({step(4 * g_quarter, 0)}));
	CHECK(f.call("project.revision").at("revision").get<int64_t>() == revision); // no undo step

	// A transpose event within the part, one before it and one after it; the root key's step and the part's own
	// transpose add to them.
	f.call("transpose.add", {{"position", 6 * g_quarter}, {"value", 3}});
	f.call("transpose.add", {{"position", 0}, {"value", -1}});
	f.call("transpose.add", {{"position", 8 * g_quarter}, {"value", 7}});
	CHECK(steps() == Json::array({step(4 * g_quarter, -1), step(6 * g_quarter, 3)}));
	f.call("project.set", {{"rootKey", "D"}});
	f.call("midiPart.set", {{"part", f.part}, {"rootKey", "C"}, {"transpose", 12}});
	CHECK(steps() == Json::array({step(4 * g_quarter, 13), step(6 * g_quarter, 17)}));
	// Muted transpose events leave the rest, which no longer changes within the part.
	f.call("transpose.setTrack", {{"mute", true}});
	CHECK(steps() == Json::array({step(4 * g_quarter, 14)}));
	f.call("midiPart.set", {{"part", f.part}, {"globalTranspose", "independent"}});
	CHECK(steps() == Json::array({step(4 * g_quarter, 12)}));

	f.call("editor.set", {{"editor", "window"}, {"indicateTranspositions", false}});
	CHECK(f.view().at("transpositions") == Json::array());
	CHECK(!f.fails("editor.set", {{"editor", "window"}, {"indicateTranspositions", "on"}}).empty());
}

TEST_CASE("the note expression settings of the key editor are view state", "[unit]")
{
	Fixture f;
	// Show Note Expression Data and Double-Click Opens Note Expression Editor are off, and the expression editor
	// starts with tuning (reference manual pp. 1303, 1308-1309).
	const Json initial = f.view();
	CHECK(initial.at("showExpression") == false);
	CHECK(initial.at("expressionDoubleClick") == false);
	CHECK(initial.at("expressionType") == "tuning");
	const int64_t revision = f.call("project.revision").at("revision").get<int64_t>();
	const Json changed = f.call("editor.set",
		{{"editor", "window"}, {"showExpression", true}, {"expressionDoubleClick", true},
			{"expressionType", "brightness"}});
	CHECK(changed.at("showExpression") == true);
	CHECK(changed.at("expressionDoubleClick") == true);
	CHECK(changed.at("expressionType") == "brightness");
	CHECK(f.view().at("expressionType") == "brightness");
	CHECK(f.call("project.revision").at("revision").get<int64_t>() == revision); // no undo step
	CHECK(!f.fails("editor.set", {{"editor", "window"}, {"expressionType", "loudness"}}).empty());
}

TEST_CASE("the MIDI editors' event colors are view state", "[unit]")
{
	// The Event Colors menu (reference manual pp. 1133-1134): the theme's note colors at first, Part or Sound Slot.
	Fixture f;
	CHECK(f.view().at("eventColors") == "default");
	const int64_t revision = f.call("project.revision").at("revision").get<int64_t>();
	CHECK(f.call("editor.set", {{"editor", "window"}, {"eventColors", "part"}}).at("eventColors") == "part");
	CHECK(f.call("editor.set", {{"editor", "window"}, {"eventColors", "soundSlot"}}).at("eventColors") == "soundSlot");
	CHECK(f.view().at("eventColors") == "soundSlot");
	CHECK(f.call("project.revision").at("revision").get<int64_t>() == revision); // no undo step
	CHECK(!f.fails("editor.set", {{"editor", "window"}, {"eventColors", "rainbow"}}).empty());
}

TEST_CASE("notes played on the MIDI inputs go into step input as chords once all their keys are up", "[unit]")
{
	// Reference manual pp. 1219-1220: editor.played, as midi.activity posts it, each note {pitch, velocity, on}.
	Fixture f;
	const auto played = [&f](const std::vector<std::vector<int>>& notes, int dropped = 0, bool cleared = false)
	{
		Json list = Json::array();
		for (const std::vector<int>& note : notes)
			list.push_back({{"pitch", note[0]}, {"velocity", note[1]}, {"on", note[2] != 0}});
		return f.call("editor.played", {{"notes", list}, {"dropped", dropped}, {"cleared", cleared}}).at("chords");
	};
	// Off, nothing goes in.
	CHECK(played({{60, 127, 1}, {60, 0, 0}}).empty());
	f.call("editor.set", {{"editor", "window"}, {"stepInput", true}, {"lengthQuantize", "1/16"}});
	// A chord: the keys pressed until all are up, with their note-on and note-off velocities.
	CHECK(played({{64, 127, 1}, {60, 0x40, 1}, {64, 127, 0}}).empty());
	const Json chords = played({{60, 64, 0}});
	REQUIRE(chords.size() == 1);
	CHECK(chords[0].size() == 2);
	f.call("editor.set", {{"editor", "window"}, {"recordReleaseVelocity", true}}); // after the chord went in
	// A pitch that is down twice, as two chord pad players play it, is up once both of its keys are (#2154).
	CHECK(played({{67, 100, 1}, {67, 100, 1}, {67, 0, 0}}).empty());
	CHECK(played({{67, 0, 0}}).size() == 1);
	const Json notes = f.partNotes();
	REQUIRE(notes.size() == 3);
	CHECK(notes[0].at("pitch") == 60);
	CHECK(notes[0].at("velocity").get<double>() == Approx(64.0 / 127.0).margin(1e-4));
	CHECK(notes[1].at("pitch") == 64);
	CHECK(notes[1].at("velocity") == 1.0);
	CHECK(notes[2].at("position") == g_sixteenth);
	CHECK(notes[2].at("pitch") == 67);
	CHECK(notes[2].at("releaseVelocity") == 0.0);
	CHECK(f.call("history.get").at("steps").back().at("action") == "Note input");
	// Each chord is an undo step of its own.
	f.call("history.undo");
	CHECK(f.partNotes().size() == 2);

	// Lost notes, such as a note-off: the keys still down are forgotten, so the next key goes in on its own, rather
	// than waiting for a key whose note-off never comes (#2154).
	CHECK(played({{48, 100, 1}}, 1).empty());
	CHECK(played({{50, 100, 1}, {50, 0, 0}}).size() == 1);
	CHECK(f.partNotes().back().at("pitch") == 50);
	// Input dropped before these notes, as when a device started (cleared): the keys down before them are forgotten,
	// and a key whose note-on comes with the loss still goes in once it is up.
	CHECK(played({{52, 100, 1}}).empty());
	CHECK(played({{53, 100, 1}}, 0, true).empty());
	const Json afterStart = played({{53, 0, 0}});
	REQUIRE(afterStart.size() == 1);
	REQUIRE(afterStart[0].size() == 1);
	CHECK(afterStart[0][0].at("pitch") == 53);
}

TEST_CASE("Right and Left Arrow move the step input cursor, and while keys are held lengthen or shorten their chord",
	"[unit]")
{
	// Reference manual p. 1220: editor.step, by steps of the quantize grid (1/16 here); the chord's notes start at the
	// length quantize's length.
	Fixture f;
	const auto step = [&f](int steps) { return f.call("editor.step", {{"editor", "window"}, {"steps", steps}}); };
	const auto note = [&f](int pitch, bool on)
	{
		const Json played = {{"pitch", pitch}, {"velocity", on ? 100 : 0}, {"on", on}};
		f.call("editor.played", {{"notes", Json::array({played})}});
	};
	CHECK_THAT(f.fails("editor.step", {{"editor", "window"}, {"steps", 1}}), ContainsSubstring("step input is off"));
	f.call("editor.set", {{"editor", "window"}, {"stepInput", true}, {"lengthQuantize", "1/16"}});
	const int64_t start = f.view().at("stepPosition").get<int64_t>(); // the part's
	// Without keys held the cursor moves: Right inserts a rest, Left goes back, not before the project's start.
	CHECK(step(1) == Json{{"held", false}, {"stepPosition", start + g_sixteenth}});
	CHECK(step(-1).at("stepPosition") == start);
	f.call("editor.set", {{"editor", "window"}, {"stepPosition", 0}});
	CHECK(step(-1).at("stepPosition") == 0);
	CHECK(f.view().at("stepPosition") == 0);
	f.call("editor.set", {{"editor", "window"}, {"stepPosition", start}});
	// Right twice while a key is held: its note is two steps longer, and the cursor moves on by three.
	note(60, true);
	CHECK(step(1) == Json{{"held", true}, {"steps", 1}});
	CHECK(step(1).at("steps") == 2);
	note(60, false);
	REQUIRE(f.partNotes().size() == 1);
	CHECK(f.partNotes()[0].at("length") == 3 * g_sixteenth);
	CHECK(f.view().at("stepPosition") == start + (3 * g_sixteenth));
	// Left shortens a quarter by a step, and the cursor moves on by one; far left leaves a step.
	f.call("editor.set", {{"editor", "window"}, {"lengthQuantize", "1/4"}});
	note(62, true);
	step(-1);
	note(62, false);
	note(64, true);
	step(-10);
	note(64, false);
	const Json notes = f.partNotes();
	REQUIRE(notes.size() == 3);
	CHECK(notes[1].at("position") == 3 * g_sixteenth);
	CHECK(notes[1].at("length") == 3 * g_sixteenth);
	CHECK(notes[2].at("position") == 4 * g_sixteenth);
	CHECK(notes[2].at("length") == g_sixteenth);
	CHECK(f.view().at("stepPosition") == start + (5 * g_sixteenth));
}

TEST_CASE("a MIDI keyboard plays into the key editor whose step or MIDI input went on last", "[integration]")
{
	// Reference manual pp. 1219-1220.
	midi::DummyBackend* backend = nullptr;
	Fixture f(dummyMidi(&backend));
	backend->addLoopback("Keys");
	backend->addLoopback("Surface"); // a control surface, outside all MIDI inputs
	f.call("audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}});
	f.call("midi.ports"); // opens the inputs
	f.call("midi.setPort", {{"id", "Surface"}, {"input", true}, {"inAllInputs", false}});
	const std::unique_ptr<midi::Output> keys = backend->openOutput("Keys");
	const std::unique_ptr<midi::Output> surface = backend->openOutput("Surface");
	const auto sendTo = [](midi::Output& output, uint8_t status, uint8_t pitch, uint8_t velocity)
	{
		audio::MidiMessage message;
		message.size = 3;
		message.bytes = {status, pitch, velocity};
		constexpr HostTime second = 1000000000;
		output.send(message, hostTime() - second);
	};
	const auto send = [&keys, &sendTo](uint8_t status, uint8_t pitch, uint8_t velocity)
	{ sendTo(*keys, status, pitch, velocity); };
	// Renders until the condition holds: offline the test runs midi.activity, which the engine runs itself while a
	// device runs; it posts editor.played, which posts editor.input.
	const auto until = [&f](const std::function<bool()>& condition)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline)
		{
			f.call("audio.render", {{"frames", 64}});
			f.call("midi.activity");
			if (condition())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return false;
	};
	const std::vector<uint64_t> ids = f.notes({{0, 60}, {g_sixteenth, 62}});
	f.call("editor.set", {{"editor", "window"}, {"midiInput", true}, {"selection", f.selection({ids[0]})}});
	// The lower zone's editor turned its step input on last: notes go there.
	f.call("editor.open", {{"editor", "lower"}, {"parts", Json::array({f.part})}});
	f.call("editor.set", {{"editor", "lower"}, {"stepInput", true}});
	f.call("editor.set", {{"editor", "lower"}, {"stepPosition", 5 * g_quarter}});
	send(0x90, 72, 100);
	send(0x80, 72, 0);
	CHECK(until([&f] { return f.partNotes().size() == 3; }));
	CHECK(f.partNotes()[2].at("pitch") == 72);
	CHECK(f.partNotes()[2].at("position") == g_quarter);
	// Its step input off, the window's MIDI input takes them: the selected note gets the pitch.
	f.call("editor.set", {{"editor", "lower"}, {"stepInput", false}});
	send(0x90, 48, 100);
	send(0x80, 48, 0);
	CHECK(until([&f, &ids] { return f.pitches({ids[0]}) == std::vector<int>{48}; }));
	CHECK(f.view().at("selection") == f.selection({ids[1]}));
	// With both off, the editors stop listening: what is played then goes nowhere, not into the next input.
	f.call("editor.set", {{"editor", "window"}, {"midiInput", false}});
	send(0x90, 50, 100);
	send(0x80, 50, 0);
	f.call("audio.render", {{"frames", 64}});
	f.call("editor.set", {{"editor", "window"}, {"midiInput", true}});
	// The control surface's notes go nowhere either: had they gone in, the selected note would get 55, not 52.
	sendTo(*surface, 0x90, 55, 100);
	sendTo(*surface, 0x80, 55, 0);
	bool surfaceReceived = false; // in before the keys' note, so that it would go into the selected note first
	for (int tries = 0; tries < 2000 && !surfaceReceived; ++tries)
	{
		f.call("audio.render", {{"frames", 64}});
		surfaceReceived = !f.call("midi.activity").at("received").empty();
		if (!surfaceReceived)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	CHECK(surfaceReceived);
	send(0x90, 52, 100);
	send(0x80, 52, 0);
	CHECK(until([&f, &ids] { return f.pitches({ids[1]}) == std::vector<int>{52}; }));
	CHECK(f.partNotes().size() == 3);

	// Step input takes what the tracks get (#2154): the on-screen keyboard's notes, and a chord pad's remote key the
	// pad's chord, C major on C1 by default (chord-pads.md), not its own pitch.
	f.call("editor.set", {{"editor", "window"}, {"midiInput", false}, {"stepInput", true}, {"keepNotes", true}});
	f.call("midi.keyboard", {{"messages", {{{"bytes", {0x90, 74, 100}}}, {{"bytes", {0x80, 74, 0}}}}}});
	CHECK(until([&f] { return f.partNotes().size() == 4; }));
	// Expression that overfills midi.activity's 256 messages, as an MPE controller's for its held notes, crowds no note
	// out: the notes have a queue of their own.
	for (int i = 0; i < 300; ++i)
		send(0xB0, 74, static_cast<uint8_t>(i % 128));
	send(0x90, 76, 100);
	send(0x80, 76, 0);
	CHECK(until([&f] { return f.partNotes().size() == 5; }));
	f.call("chordPads.set", {{"remote", true}});
	f.call("audio.render", {{"frames", 64}}); // the new graph
	send(0x90, 36, 100);
	send(0x80, 36, 0);
	CHECK(until([&f] { return f.partNotes().size() == 9; }));
	std::vector<int> pitches;
	for (const Json& note : f.partNotes())
		pitches.push_back(note.at("pitch").get<int>());
	std::sort(pitches.begin(), pitches.end());
	CHECK(
		pitches == std::vector<int>{48, 48, 52, 60, 64, 67, 72, 74, 76}); // the first 48 is the note MIDI input changed

	// A device start drops the input that waited, and the next post says so before its notes: a key held past the run
	// of midi.activity that took its note-on still goes in once it is up.
	f.call("audio.start");
	f.call("audio.stop");
	send(0x90, 84, 100);
	bool noteOnTaken = false;
	for (int tries = 0; tries < 2000 && !noteOnTaken; ++tries)
	{
		f.call("audio.render", {{"frames", 64}});
		noteOnTaken = !f.call("midi.activity").at("received").empty();
		if (!noteOnTaken)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	REQUIRE(noteOnTaken);
	CHECK(f.partNotes().size() == 9);
	send(0x80, 84, 0);
	CHECK(until([&f] { return f.partNotes().size() == 10; }));
}
