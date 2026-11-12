// SPDX-License-Identifier: GPL-3.0-or-later
// The chord pads and the chord assistant (#174, reference manual pp. 1346-1391): the music theory of presets, degrees,
// tensions, inversions and suggestions, the chordPads.* commands with undo and the project file, pads that play and
// record, step input, pads dragged onto tracks and the pads' remote keys on a MIDI input.
#include "engine/document/chordpads.h"
#include "engine/engine.h"
#include "engine/midi/padvoices.h"
#include "engine/midifile/smf.h"
#include "mcp/server.h"
#include "platform/audio/device.h"
#include "platform/hosttime.h"
#include "platform/midi/dummyport.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;

namespace
{
	constexpr int64_t g_beatTicks = 1720320;
	constexpr int64_t g_bar = 4 * g_beatTicks; // 4/4

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	std::vector<std::string> names(const std::vector<midi::ChordSymbol>& chords)
	{
		std::vector<std::string> result;
		result.reserve(chords.size());
		for (const midi::ChordSymbol& chord : chords)
			result.push_back(midi::chordName(chord));
		return result;
	}

	midi::ChordSymbol chord(const std::string& text)
	{
		const std::optional<midi::ChordSymbol> parsed = midi::parseChord(text);
		REQUIRE(parsed.has_value());
		return parsed.value_or(midi::ChordSymbol());
	}

	std::vector<std::string> padChords(Client& client)
	{
		std::vector<std::string> result;
		const Json pads = run(client, "chordPads.get");
		for (const Json& pad : pads.at("pads"))
			result.push_back(pad.at("chord").get<std::string>());
		return result;
	}

	std::vector<std::vector<int>> sorted(std::vector<std::vector<int>> notes)
	{
		std::sort(notes.begin(), notes.end());
		return notes;
	}

	Json pad(Client& client, int index)
	{
		return run(client, "chordPads.get").at("pads").at(static_cast<size_t>(index));
	}

	// The note-ons and note-offs the instruments played, as [status with channel, pitch, velocity].
	std::vector<std::vector<int>> notesPlayed(const Json& capture)
	{
		std::vector<std::vector<int>> notes;
		for (const Json& captured : capture.at("captured"))
		{
			const Json& bytes = captured.at("bytes");
			const int status = bytes.at(0).get<int>() & 0xF0;
			if (status == 0x90 || status == 0x80)
				notes.push_back({bytes.at(0).get<int>(), bytes.at(1).get<int>(), bytes.at(2).get<int>()});
		}
		return notes;
	}

	Json waitFor(Client& client, const std::string& name)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const std::optional<Message> next = client.wait(std::chrono::milliseconds(50));
			const auto* event = next ? std::get_if<Event>(&*next) : nullptr;
			if (event != nullptr && event->name == name)
				return event->data;
		}
		return nullptr;
	}

	// An instrument track playing the note probe that monitors, on the dummy device.
	struct Rig
	{
		explicit Rig(std::vector<std::unique_ptr<gnubase::midi::Backend>> midi = {})
			: engine(gnubase::audio::makeBackends(), {}, std::move(midi))
		{
			run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}});
			const Json added = run(client, "track.add", {{"type", "instrument"}});
			track = added.at("track").at("id").get<ObjectId>();
			channel = added.at("channel").at("id").get<ObjectId>();
			run(client, "channel.set", {{"channel", channel}, {"instrument", "noteProbe"}, {"monitor", true}});
			run(client, "audio.render", {{"frames", 64}}); // the new graph
			run(client, "midi.capture", {{"start", true}});
		}

		std::vector<std::vector<int>> played()
		{
			run(client, "audio.render", {{"frames", 64}});
			return notesPlayed(run(client, "midi.capture"));
		}

		Engine engine;
		Client client{engine};
		ObjectId track = 0;
		ObjectId channel = 0;
	};
}

TEST_CASE("scale-based presets, degrees, tensions, inversions and the assistant's suggestions", "[unit]")
{
	using namespace gnubase::engine::chordpads;
	CHECK(names(presetChords("major", 0)) == std::vector<std::string>{"C", "Dm", "Em", "F", "G", "Am", "Bdim"});
	CHECK(names(presetChords("dorian", 2)) == std::vector<std::string>{"Dm", "Em", "F", "G", "Am", "Bdim", "C"});
	CHECK(names(presetChords("minor", 9)) == std::vector<std::string>{"Am", "Bdim", "C", "Dm", "Em", "F", "G"});
	CHECK(presetChords("blues", 0).empty());

	CHECK(degreeName(chord("C"), 0) == "I");
	CHECK(degreeName(chord("G7"), 0) == "V");
	CHECK(degreeName(chord("Dm"), 0) == "ii");
	CHECK(degreeName(chord("Bdim"), 0) == "vii\xC2\xB0");
	CHECK(degreeName(chord("A#"), 0) == "bVII");
	CHECK(degreeName(chord("Am"), 9, "minor") == "i");
	CHECK(degreeName(chord("G"), 9, "minor") == "VII");
	// In a preset's scale: F in D dorian is the third degree, not a flat one.
	CHECK(degreeName(chord("F"), 2, "dorian") == "III");
	CHECK(degreeName(chord("Caug"), 0) == "I+");

	// More tensions follow the type's ladder; on the key's fifth degree a major chord gets the dominant seventh.
	CHECK(midi::chordName(withTensions(chord("C"), 1, 0)) == "Cmaj7");
	CHECK(midi::chordName(withTensions(chord("C"), 2, 0)) == "Cmaj7(9)");
	CHECK(midi::chordName(withTensions(chord("G"), 1, 0)) == "G7");
	CHECK(midi::chordName(withTensions(chord("Dm"), 3, 0)) == "Dm7(9,11)");
	CHECK(midi::chordName(withTensions(chord("Bdim"), 1, 0)) == "Bm7b5");
	CHECK(midi::chordName(withTensions(chord("Cmaj7(9)"), -1, 0)) == "Cmaj7");
	CHECK(midi::chordName(withTensions(chord("C"), -1, 0)) == "C");
	CHECK(midi::chordName(transposed(chord("C/E"), 2)) == "D/F#");
	CHECK(midi::chordName(transposed(chord("C"), -1)) == "B");

	CHECK(inverted({48, 60, 64, 67}, 1) == std::vector<int>{48, 64, 67, 72});
	CHECK(inverted({48, 60, 64, 67}, 2) == std::vector<int>{48, 67, 72, 76});
	CHECK(inverted({48, 60, 64, 67}, -1) == std::vector<int>{48, 55, 60, 64});

	// After C in C major, the chord a fifth down comes first; all of the key's chords before those outside it.
	const std::vector<Suggestion> cadence = suggestions(chord("C"), 0, "major", "cadence", 2);
	REQUIRE(cadence.size() == 24);
	CHECK(midi::chordName(cadence[0].chord) == "F");
	CHECK(cadence[0].level == 0);
	CHECK(std::is_sorted(
		cadence.begin(), cadence.end(), [](const Suggestion& a, const Suggestion& b) { return a.level < b.level; }));
	CHECK(std::none_of(
		cadence.begin(), cadence.end(), [](const Suggestion& s) { return midi::chordName(s.chord) == "C"; }));
	// Common notes: chords that share exactly two notes with C, such as Am and Em.
	const std::vector<Suggestion> common = suggestions(chord("C"), 0, "major", "commonNotes", 2);
	std::vector<std::string> commonNames;
	commonNames.reserve(common.size());
	for (const Suggestion& suggestion : common)
		commonNames.push_back(midi::chordName(suggestion.chord));
	CHECK(std::find(commonNames.begin(), commonNames.end(), "Am") != commonNames.end());
	CHECK(std::find(commonNames.begin(), commonNames.end(), "Em") != commonNames.end());
	CHECK(std::find(commonNames.begin(), commonNames.end(), "F") == commonNames.end());
	// The levels are measured against the scale: in D dorian Dm is common, D (with F#) isn't, as it is in D major.
	const auto levelOf = [](const std::vector<Suggestion>& list, const std::string& name)
	{
		const auto found = std::find_if(
			list.begin(), list.end(), [&name](const Suggestion& s) { return midi::chordName(s.chord) == name; });
		return found == list.end() ? -1 : found->level;
	};
	CHECK(levelOf(suggestions(chord("Am"), 2, "dorian", "commonNotes", 1), "Dm") == 0);
	CHECK(levelOf(suggestions(chord("Am"), 2, "dorian", "commonNotes", 1), "D") == 1);
	CHECK(levelOf(suggestions(chord("Am"), 2, "major", "commonNotes", 1), "D") == 0);
	CHECK(levelOf(suggestions(chord("G"), 2, "dorian", "cadence", 2), "F") == 0);
}

TEST_CASE("chord pads hold chords with voicings and change with the pad modifiers", "[unit]")
{
	Engine engine;
	Client client(engine);
	// A new project has twelve pads with the major preset on C and the remote keys C1 to B1.
	const Json pads = run(client, "chordPads.get");
	REQUIRE(pads.at("pads").size() == 12);
	CHECK(padChords(client) == std::vector<std::string>{"C", "Dm", "Em", "F", "G", "Am", "Bdim", "", "", "", "", ""});
	CHECK(pads.at("pads")[0].at("pitches") == std::vector<int>{48, 60, 64, 67});
	CHECK(pads.at("pads")[4].at("degree") == "V");
	CHECK(pads.at("pads")[0].at("key") == 36);
	CHECK(pads.at("pads")[7].at("pitches").empty());
	CHECK(pads.at("remote") == false);

	// Assigning: by name, by the pitches of a chord, and removing.
	run(client, "chordPads.assign",
		{{"assignments",
			{{{"pad", 7}, {"chord", "Am7"}}, {{"pad", 8}, {"pitches", {64, 67, 72}}}, {{"pad", 1}, {"chord", ""}}}}});
	CHECK(pad(client, 7).at("chord") == "Am7");
	CHECK(pad(client, 8).at("chord") == "C/E");
	CHECK(pad(client, 1).at("chord").get<std::string>().empty());
	CHECK_THAT(reject(client, "chordPads.assign", {{"assignments", {{{"pad", 12}, {"chord", "C"}}}}}),
		ContainsSubstring("no chord pad 12"));
	CHECK_THAT(reject(client, "chordPads.assign", {{"assignments", {{{"pad", 0}, {"chord", "H"}}}}}),
		ContainsSubstring("unknown chord"));

	// The modifiers on several pads: unassigned pads stay as they are.
	run(client, "chordPads.modify", {{"pads", {0, 1, 4}}, {"transpose", 2}});
	CHECK(padChords(client)[0] == "D");
	CHECK(padChords(client)[1] == "");
	CHECK(padChords(client)[4] == "A");
	run(client, "chordPads.modify", {{"pads", {0}}, {"tensions", 1}});
	CHECK(pad(client, 0).at("chord") == "Dmaj7");
	run(client, "chordPads.modify", {{"pads", {0}}, {"tensions", -1}, {"transpose", -2}});
	CHECK(pad(client, 0).at("chord") == "C");
	run(client, "chordPads.modify", {{"pads", {0}}, {"voicing", 1}});
	CHECK(pad(client, 0).at("pitches") == std::vector<int>{48, 64, 67, 72});
	CHECK(pad(client, 0).at("adaptive") == false);

	// Locked pads don't change.
	run(client, "chordPads.setPad", {{"pad", 0}, {"locked", true}});
	run(client, "chordPads.modify", {{"pads", {0}}, {"transpose", 1}});
	CHECK(pad(client, 0).at("chord") == "C");
	CHECK_THAT(reject(client, "chordPads.assign", {{"assignments", {{{"pad", 0}, {"chord", "F"}}}}}),
		ContainsSubstring("locked"));
	run(client, "chordPads.setPad", {{"pad", 0}, {"locked", false}, {"adaptive", true}});

	// Adaptive voicing: with C in its first inversion as the reference, Am takes the inversion nearest to it.
	run(client, "chordPads.setPad", {{"pad", 0}, {"reference", true}});
	CHECK(run(client, "chordPads.get").at("reference") == 0);
	CHECK(pad(client, 5).at("pitches") == std::vector<int>{57, 64, 69, 72});
	run(client, "chordPads.setPad", {{"pad", 5}, {"adaptive", false}});
	CHECK(pad(client, 5).at("pitches") == std::vector<int>{57, 69, 72, 76});

	// Swapping and copying keep the reference on its pad.
	run(client, "chordPads.move", {{"from", 0}, {"to", 2}});
	CHECK(padChords(client)[0] == "Em");
	CHECK(padChords(client)[2] == "C");
	CHECK(run(client, "chordPads.get").at("reference") == 0);
	run(client, "chordPads.move", {{"from", 2}, {"to", 3}, {"copy", true}});
	CHECK(padChords(client)[3] == "C");
	CHECK(padChords(client)[2] == "C");

	// Each change is an undo step, and undo and redo announce the pads too.
	while (client.poll())
	{
	}
	run(client, "history.undo");
	CHECK(padChords(client)[3] == "F");
	const Json undone = waitFor(client, "chordPads.changed");
	REQUIRE(undone.is_object());
	CHECK(undone.at("pads")[3].at("chord") == "F");
	run(client, "history.redo");
	const Json redone = waitFor(client, "chordPads.changed");
	REQUIRE(redone.is_object());
	CHECK(redone.at("pads")[3].at("chord") == "C");
}

TEST_CASE("the root key, presets and the chord track's chords fill the pads", "[unit]")
{
	Engine engine;
	Client client(engine);
	// The root key without transposing changes the degrees only; with Transpose Pads the chords move, the nearer way.
	run(client, "chordPads.set", {{"rootKey", 7}});
	CHECK(pad(client, 0).at("chord") == "C");
	CHECK(pad(client, 0).at("degree") == "IV");
	run(client, "chordPads.set", {{"rootKey", 9}, {"transposePads", true}});
	CHECK(pad(client, 0).at("chord") == "D");
	CHECK(pad(client, 0).at("degree") == "IV");

	run(client, "chordPads.loadPreset", {{"preset", "dorian"}, {"root", 2}});
	CHECK(padChords(client) == std::vector<std::string>{"Dm", "Em", "F", "G", "Am", "Bdim", "C", "", "", "", "", ""});
	const Json pads = run(client, "chordPads.get");
	CHECK(pads.at("rootKey") == 2);
	CHECK(pads.at("preset") == "dorian");
	// The degrees and the assistant's levels are the loaded preset's scale's.
	CHECK(pads.at("pads")[0].at("degree") == "i");
	CHECK(pads.at("pads")[2].at("degree") == "III");
	// Enh. Shift (p. 1363): spelled with flats.
	CHECK(chordpads::withFlats("F#m7/C#") == "Gbm7/Db");
	CHECK(chordpads::withFlats("C7(#9)") == "C7(#9)");
	CHECK(chordpads::withFlats("E#") == "F");
	CHECK(pads.at("pads")[0].at("spelled") == "Dm");
	// And the Nashville numbers (p. 1390).
	CHECK(pads.at("pads")[0].at("nashville") == "1m");
	CHECK(pads.at("pads")[2].at("nashville") == "3");
	CHECK(pads.at("pads")[5].at("nashville") == "6\xC2\xB0");
	CHECK(pads.at("pads")[7].at("nashville") == "");
	const Json afterDm = run(client, "chordPads.assistant", {{"kind", "list"}, {"chord", "Dm"}});
	const auto f = std::find_if(afterDm.at("suggestions").begin(), afterDm.at("suggestions").end(),
		[](const Json& suggestion) { return suggestion.at("chord") == "F"; });
	REQUIRE(f != afterDm.at("suggestions").end());
	CHECK(f->at("level") == 0);
	CHECK(f->at("degree") == "III");
	// Players only keeps the chords; the basic player has no octave offset.
	run(client, "chordPads.set", {{"octaveOffset", 1}});
	run(client, "chordPads.loadPreset", {{"preset", "major"}, {"load", "players"}});
	CHECK(padChords(client)[0] == "Dm");
	CHECK(run(client, "chordPads.get").at("octaveOffset") == 0);
	CHECK_THAT(reject(client, "chordPads.loadPreset", {{"preset", "blues"}}), ContainsSubstring("preset"));

	// Assign from Chord Track: each chord once, in the track's order.
	CHECK_THAT(reject(client, "chordPads.fromChordTrack"), ContainsSubstring("no chords"));
	for (const auto& [position, name] : std::vector<std::pair<int64_t, std::string>>{
			 {0, "C"}, {g_bar, "Am"}, {2 * g_bar, "C"}, {3 * g_bar, "X"}, {4 * g_bar, "G7"}})
		run(client, "chord.add", {{"position", position}, {"chord", name}});
	run(client, "chordPads.fromChordTrack");
	CHECK(padChords(client) == std::vector<std::string>{"C", "Am", "G7", "", "", "", "", "", "", "", "", ""});

	run(client, "chordPads.set", {{"padCount", 16}, {"remoteStart", 48}, {"remoteEnd", 50}});
	const Json more = run(client, "chordPads.get");
	CHECK(more.at("pads").size() == 16);
	CHECK(more.at("pads")[2].at("key") == 50);
	CHECK(more.at("pads")[3].at("key").is_null());
	CHECK_THAT(reject(client, "chordPads.set", {{"padCount", 33}}), ContainsSubstring("padCount"));
	CHECK_THAT(reject(client, "chordPads.set", {{"remoteStart", 60}, {"remoteEnd", 50}}), ContainsSubstring("remote"));
}

TEST_CASE("piano and guitar players voice the pads in their styles, and the selected player plays", "[unit]")
{
	// Reference manual pp. 1376-1377: the Player Settings pane's players.
	Engine engine;
	Client client(engine);
	const auto voiced = [](const std::string& name, const std::string& library, const std::string& style)
	{
		midi::VoicingStyle voicing;
		voicing.library = library;
		voicing.style = style;
		return midi::voiceChord(chord(name), {0, false}, {}, voicing);
	};
	Json pads = run(client, "chordPads.get");
	REQUIRE(pads.at("players").size() == 1);
	CHECK(pads.at("players")[0].at("library") == "basic");
	CHECK(pads.at("player") == 0);
	run(client, "chordPads.addPlayer", {{"library", "piano"}});
	pads = run(client, "chordPads.get");
	CHECK(pads.at("player") == 1);
	CHECK(pads.at("players")[1].at("name") == "Piano Player");
	CHECK(pads.at("pads")[0].at("pitches") == voiced("C", "piano", ""));
	run(client, "chordPads.setPlayer", {{"style", "fourNote"}, {"name", "Keys"}});
	CHECK(pad(client, 0).at("pitches") == voiced("C", "piano", "fourNote"));
	CHECK(run(client, "chordPads.get").at("players")[1].at("name") == "Keys");
	CHECK_THAT(reject(client, "chordPads.set", {{"octaveOffset", 1}}), ContainsSubstring("basic player"));
	CHECK_THAT(reject(client, "chordPads.setPlayer", {{"style", "modernJazz"}}), ContainsSubstring("voicing style"));
	run(client, "chordPads.addPlayer", {{"library", "guitar"}, {"style", "threeString"}});
	CHECK(pad(client, 4).at("pitches") == voiced("G", "guitar", "threeString"));
	// The voicing modifiers invert the selected player's voicing, and only its (p. 1378).
	run(client, "chordPads.modify", {{"pads", {4}}, {"voicing", 1}});
	CHECK(pad(client, 4).at("pitches") == chordpads::inverted(voiced("G", "guitar", "threeString"), 1));
	CHECK(pad(client, 4).at("voicings") == std::vector<int>{0, 0, 1});
	run(client, "chordPads.set", {{"player", 1}});
	CHECK(pad(client, 4).at("voicing") == 0);
	run(client, "chordPads.set", {{"player", 2}});
	// Back to the basic player, whose octave offset changes its voicings only.
	run(client, "chordPads.set", {{"player", 0}, {"octaveOffset", 1}});
	CHECK(pad(client, 0).at("pitches") == std::vector<int>{60, 72, 76, 79});
	run(client, "chordPads.removePlayer", {{"player", 1}});
	pads = run(client, "chordPads.get");
	CHECK(pads.at("players").size() == 2);
	CHECK(pads.at("players")[1].at("library") == "guitar");
	CHECK(pads.at("player") == 0);
	run(client, "chordPads.removePlayer", {{"player", 1}});
	CHECK_THAT(reject(client, "chordPads.removePlayer"), ContainsSubstring("keep one player"));
	CHECK_THAT(reject(client, "chordPads.set", {{"player", 3}}), ContainsSubstring("no chord pads player"));
	// A preset with players brings them back; a scale-based one loads the basic player alone.
	run(client, "chordPads.addPlayer", {{"library", "piano"}});
	run(client, "chordPads.savePreset", {{"name", "Two"}});
	run(client, "chordPads.loadPreset", {{"preset", "major"}});
	CHECK(run(client, "chordPads.get").at("players").size() == 1);
	run(client, "chordPads.loadPreset", {{"preset", "Two"}, {"load", "players"}});
	pads = run(client, "chordPads.get");
	CHECK(pads.at("players").size() == 2);
	CHECK(pads.at("player") == 1);
	// A file from before the players keeps its octave offset.
	Project project;
	chordPadsFromJson(project, {{"octaveOffset", -2}});
	REQUIRE(project.chordPads.players.size() == 1);
	CHECK(project.chordPads.players[0].octaveOffset == -2);
}

TEST_CASE("the user's own chord pads presets are saved, listed, loaded and deleted", "[unit]")
{
	// Reference manual pp. 1348, 1371: Save Preset keeps the chords and the player, Previous and Next Preset step
	// through the list that chordPads.presets gives.
	Engine engine;
	Client client(engine);
	std::vector<std::string> names = run(client, "chordPads.presets").at("presets").get<std::vector<std::string>>();
	CHECK(names == std::vector<std::string>{"major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian"});
	run(client, "chordPads.loadPreset", {{"preset", "dorian"}, {"root", 2}});
	run(client, "chordPads.assign", {{"assignments", {{{"pad", 8}, {"chord", "A7"}}}}});
	run(client, "chordPads.modify", {{"pads", {1}}, {"voicing", 1}});
	run(client, "chordPads.set", {{"octaveOffset", -1}});
	const Json saved = run(client, "chordPads.get").at("pads");
	const auto steps = run(client, "history.get").at("position");
	run(client, "chordPads.savePreset", {{"name", "Song"}});
	CHECK(run(client, "history.get").at("position") == steps); // a preset is no change to the project
	names = run(client, "chordPads.presets").at("presets").get<std::vector<std::string>>();
	CHECK(names.back() == "Song");
	CHECK_THAT(reject(client, "chordPads.savePreset", {{"name", "major"}}), ContainsSubstring("built-in"));
	CHECK_THAT(reject(client, "chordPads.savePreset", {{"name", ""}}), ContainsSubstring("name"));

	// Loaded, it brings back the chords pad by pad with their voicings, the root key, the scale and the player.
	run(client, "chordPads.loadPreset", {{"preset", "major"}});
	run(client, "chordPads.loadPreset", {{"preset", "Song"}, {"load", "chords"}});
	CHECK(run(client, "chordPads.get").at("pads") != saved); // the player isn't loaded
	CHECK(run(client, "chordPads.get").at("octaveOffset") == 0);
	run(client, "chordPads.loadPreset", {{"preset", "Song"}, {"load", "players"}});
	const Json loaded = run(client, "chordPads.get");
	CHECK(loaded.at("pads") == saved);
	CHECK(loaded.at("rootKey") == 2);
	CHECK(loaded.at("preset") == "Song");
	CHECK(loaded.at("pads")[2].at("degree") == "III"); // in D dorian

	// Deleted, it is gone.
	run(client, "chordPads.deletePreset", {{"name", "Song"}});
	CHECK(run(client, "chordPads.presets").at("presets").size() == 7);
	CHECK_THAT(reject(client, "chordPads.loadPreset", {{"preset", "Song"}}), ContainsSubstring("no chord pads preset"));
	CHECK_THAT(reject(client, "chordPads.deletePreset", {{"name", "Song"}}), ContainsSubstring("no chord pads preset"));
}

TEST_CASE("a user preset loaded in another project brings back its players without their tracks", "[unit]")
{
	// Presets are the user's, for every project; track ids are each project's.
	Engine engine;
	Client client(engine);
	const auto track = run(client, "track.add", {{"type", "instrument"}}).at("track").at("id").get<ObjectId>();
	run(client, "chordPads.addPlayer", {{"library", "piano"}, {"track", track}});
	run(client, "chordPads.savePreset", {{"name", "Tracked"}});
	run(client, "project.new");
	run(client, "track.add", {{"type", "audio"}});
	run(client, "chordPads.loadPreset", {{"preset", "Tracked"}, {"load", "players"}});
	const Json players = run(client, "chordPads.get").at("players");
	REQUIRE(players.size() == 2);
	CHECK(players[1].at("library") == "piano");
	CHECK(players[0].at("track") == 0);
	CHECK(players[1].at("track") == 0);
	run(client, "chordPads.deletePreset", {{"name", "Tracked"}});
}

TEST_CASE("the chord assistant suggests chords after a pad's and shows the circle of fifths", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json list = run(client, "chordPads.assistant", {{"kind", "list"}, {"chord", "C"}});
	REQUIRE(list.at("suggestions").size() == 24);
	CHECK(list.at("suggestions")[0] == Json{{"chord", "F"}, {"degree", "IV"}, {"level", 0}});
	const Json common =
		run(client, "chordPads.assistant", {{"kind", "list"}, {"chord", "C"}, {"algorithm", "commonNotes"}});
	CHECK(!common.at("suggestions").empty());

	const Json circle = run(client, "chordPads.assistant", {{"kind", "circle"}});
	REQUIRE(circle.at("outer").size() == 12);
	CHECK(circle.at("outer")[0] == Json{{"chord", "C"}, {"degree", "I"}});
	CHECK(circle.at("outer")[1] == Json{{"chord", "G"}, {"degree", "V"}});
	CHECK(circle.at("outer")[11] == Json{{"chord", "F"}, {"degree", "IV"}});
	CHECK(circle.at("outer")[2] == Json{{"chord", "D"}, {"degree", ""}});
	CHECK(circle.at("inner")[0] == Json{{"chord", "Am"}, {"degree", "vi"}});
	// In A minor the relative major is first, and the degrees are the minor key's.
	run(client, "chordPads.set", {{"rootKey", 9}});
	const Json minor = run(client, "chordPads.assistant", {{"kind", "circle"}, {"minor", true}});
	CHECK(minor.at("outer")[0] == Json{{"chord", "C"}, {"degree", "III"}});
	CHECK(minor.at("inner")[0] == Json{{"chord", "Am"}, {"degree", "i"}});
}

TEST_CASE("the Proximity assistant and the List assistant's cadence types", "[unit]")
{
	// Reference manual pp. 1373-1374 (#1089).
	Engine engine;
	Client client(engine);
	const Json near = run(client, "chordPads.assistant", {{"kind", "proximity"}, {"chord", "C"}}).at("suggestions");
	REQUIRE(near.size() == 24);
	const auto distanceOf = [&near](const std::string& name)
	{
		for (const Json& suggestion : near)
			if (suggestion.at("chord") == name)
				return suggestion.at("distance").get<int>();
		return -1;
	};
	// A chord that shares two notes is nearest, one with a seventh a step further, one that shares one note further.
	CHECK(distanceOf("Am") == 1);
	CHECK(distanceOf("Em") == 1);
	CHECK(distanceOf("Cmaj7") == 2);
	CHECK(distanceOf("F") == 2);
	CHECK(near.front().at("distance") == 1);
	CHECK(near.back().at("distance") >= near.front().at("distance"));
	for (const Json& suggestion : near)
		CHECK(suggestion.contains("level"));

	// Cadence types keep the chords of one harmonic function in the key: the dominant's are on V and vii.
	const Json dominant =
		run(client, "chordPads.assistant", {{"kind", "list"}, {"chord", "C"}, {"cadenceType", "dominant"}})
			.at("suggestions");
	REQUIRE_FALSE(dominant.empty());
	for (const Json& suggestion : dominant)
	{
		std::string numeral;
		for (const char letter : suggestion.at("degree").get<std::string>())
			if (letter == 'I' || letter == 'V' || letter == 'i' || letter == 'v')
				numeral += static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
		INFO(suggestion.at("chord"));
		CHECK((numeral == "V" || numeral == "VII"));
	}
	const Json tonic = run(client, "chordPads.assistant", {{"kind", "list"}, {"chord", "G"}, {"cadenceType", "tonic"}})
						   .at("suggestions");
	CHECK(
		std::any_of(tonic.begin(), tonic.end(), [](const Json& suggestion) { return suggestion.at("chord") == "C"; }));
	CHECK(
		std::none_of(tonic.begin(), tonic.end(), [](const Json& suggestion) { return suggestion.at("chord") == "F"; }));
	CHECK_THAT(reject(client, "chordPads.assistant", {{"kind", "list"}, {"chord", "C"}, {"cadenceType", "plagal"}}),
		ContainsSubstring("cadenceType"));
}

TEST_CASE("triggered pads play into all MIDI inputs, one channel per voice, and are recorded", "[integration]")
{
	Rig rig;
	Client& client = rig.client;
	const Json triggered = run(client, "chordPads.trigger", {{"pad", 0}, {"velocity", 90}});
	CHECK(triggered.at("pitches") == std::vector<int>{48, 60, 64, 67});
	CHECK(
		rig.played() == std::vector<std::vector<int>>{{0x93, 48, 90}, {0x92, 60, 90}, {0x91, 64, 90}, {0x90, 67, 90}});
	// The off ends what the on played, even after the pad changed.
	run(client, "chordPads.assign", {{"assignments", {{{"pad", 0}, {"chord", "F"}}}}});
	run(client, "chordPads.trigger", {{"pad", 0}, {"on", false}});
	CHECK(rig.played() == std::vector<std::vector<int>>{{0x83, 48, 0}, {0x82, 60, 0}, {0x81, 64, 0}, {0x80, 67, 0}});
	// An assistant's chord auditions at the pads' velocity.
	run(client, "chordPads.trigger", {{"chord", "Am"}});
	run(client, "chordPads.trigger", {{"chord", "Am"}, {"on", false}});
	CHECK(rig.played().size() == 8);
	CHECK_THAT(reject(client, "chordPads.trigger", {{"pad", 20}}), ContainsSubstring("no chord pad"));

	// A record-enabled track records the pad's chord.
	run(client, "channel.set", {{"channel", rig.channel}, {"monitor", false}, {"recordEnable", true}});
	run(client, "transport.record");
	run(client, "chordPads.trigger", {{"pad", 5}, {"velocity", 127}});
	run(client, "audio.render", {{"frames", 640}});
	run(client, "chordPads.trigger", {{"pad", 5}, {"on", false}});
	run(client, "audio.render", {{"frames", 640}});
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(waitFor(client, "recording.added").is_object());
	std::vector<int> recorded;
	const Json project = run(client, "project.get");
	for (const Json& part : project.at("midiParts"))
		for (const Json& note : part.at("notes"))
			recorded.push_back(note.at("pitch").get<int>());
	std::sort(recorded.begin(), recorded.end());
	CHECK(recorded == std::vector<int>{57, 69, 72, 76});
}

TEST_CASE("triggering a pad plays notes, so MCP neither calls it read-only nor lists it as a resource", "[unit]")
{
	Engine engine;
	gnubase::mcp::Server server(engine);
	const auto respond = [&server](const std::string& method)
	{
		const std::optional<Json> response =
			server.handle(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", Json::object()}});
		REQUIRE(response.has_value());
		return response.value_or(Json()).at("result");
	};
	const Json tools = respond("tools/list").at("tools");
	const auto trigger = std::find_if(
		tools.begin(), tools.end(), [](const Json& tool) { return tool.at("name") == "chordPads_trigger"; });
	REQUIRE(trigger != tools.end());
	CHECK(trigger->at("annotations").at("readOnlyHint") == false);
	const Json resources = respond("resources/list").at("resources");
	CHECK(std::none_of(resources.begin(), resources.end(),
		[](const Json& resource) { return resource.at("uri") == "gnubase://chordPads.trigger"; }));
}

TEST_CASE("a MIDI input's notes on the remote keys trigger the pads while the remote is on", "[integration]")
{
	auto dummy = std::make_unique<gnubase::midi::DummyBackend>();
	gnubase::midi::DummyBackend& backend = *dummy;
	backend.addLoopback("Keys");
	std::vector<std::unique_ptr<gnubase::midi::Backend>> midi;
	midi.push_back(std::move(dummy));
	Rig rig(std::move(midi));
	Client& client = rig.client;
	run(client, "midi.ports"); // opens the inputs
	const auto keys = backend.openOutput("Keys");
	constexpr gnubase::HostTime second = 1000000000;
	const auto send = [&](uint8_t status, uint8_t pitch, uint8_t velocity)
	{
		gnubase::audio::MidiMessage message;
		message.size = 3;
		message.bytes = {status, pitch, velocity};
		keys->send(message, gnubase::hostTime() - second);
		return rig.played();
	};
	// Off by default: C1 plays itself.
	CHECK(send(0x90, 36, 100) == std::vector<std::vector<int>>{{0x90, 36, 100}});
	CHECK(send(0x80, 36, 0) == std::vector<std::vector<int>>{{0x80, 36, 0}});

	run(client, "chordPads.set", {{"remote", true}});
	run(client, "audio.render", {{"frames", 64}}); // the new graph
	CHECK(send(0x90, 36, 80) ==
		std::vector<std::vector<int>>{{0x93, 48, 80}, {0x92, 60, 80}, {0x91, 64, 80}, {0x90, 67, 80}});
	// The note-off ends the chord the note-on played, also when it comes as a note-on without velocity.
	run(client, "chordPads.assign", {{"assignments", {{{"pad", 0}, {"chord", "F"}}}}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(
		send(0x90, 36, 0) == std::vector<std::vector<int>>{{0x83, 48, 0}, {0x82, 60, 0}, {0x81, 64, 0}, {0x80, 67, 0}});
	// A key of an unassigned pad, and keys outside the range, play themselves.
	CHECK(send(0x90, 47, 100) == std::vector<std::vector<int>>{{0x90, 47, 100}});
	CHECK(send(0x90, 60, 100) == std::vector<std::vector<int>>{{0x90, 60, 100}});
	// D1 plays Dm.
	CHECK(send(0x90, 38, 70).size() == 4);
	CHECK(send(0x80, 38, 0).size() == 4);

	// Filter Notes (reference manual p. 1379): the keys of unassigned pads play nothing, the others still themselves.
	run(client, "chordPads.set", {{"filterNotes", true}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(send(0x90, 47, 100).empty());
	CHECK(send(0x80, 47, 0).empty());
	CHECK(send(0x90, 60, 100) == std::vector<std::vector<int>>{{0x90, 60, 100}});
	// Latch Chords (p. 1386): a key's chord plays until the key comes again; its note-off ends nothing.
	run(client, "chordPads.set", {{"latch", true}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(send(0x90, 36, 80).size() == 4);
	CHECK(send(0x80, 36, 0).empty());
	CHECK(sorted(send(0x90, 36, 80)) == sorted({{0x83, 53, 0}, {0x82, 65, 0}, {0x81, 69, 0}, {0x80, 72, 0}}));
	CHECK(send(0x80, 36, 0).empty());
	// With Chord Pad Output Mode off the pads play only into tracks whose MIDI input is the chord pads.
	run(client, "chordPads.set", {{"outputMode", false}, {"latch", false}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(send(0x90, 36, 80).empty());
	CHECK(send(0x80, 36, 0).empty());
	run(client, "channel.set", {{"channel", rig.channel}, {"midiInput", "chordPads"}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(send(0x90, 36, 80).size() == 4);
	CHECK(send(0x80, 36, 0).size() == 4);
	CHECK(send(0x90, 60, 100).empty()); // the other keys don't reach it
}

namespace
{
	// A rig with two MIDI inputs, Keys and Other, and the pads' remote on.
	struct RemoteRig
	{
		RemoteRig() : rig(backends())
		{
			run(rig.client, "midi.ports"); // opens the inputs
			keys = backend->openOutput("Keys");
			other = backend->openOutput("Other");
			run(rig.client, "chordPads.set", {{"remote", true}});
			run(rig.client, "audio.render", {{"frames", 64}});
		}

		std::vector<std::unique_ptr<gnubase::midi::Backend>> backends()
		{
			auto dummy = std::make_unique<gnubase::midi::DummyBackend>();
			backend = dummy.get();
			backend->addLoopback("Keys");
			backend->addLoopback("Other");
			std::vector<std::unique_ptr<gnubase::midi::Backend>> midi;
			midi.push_back(std::move(dummy));
			return midi;
		}

		// Sends a note from the input and returns what the instrument played.
		std::vector<std::vector<int>> send(uint8_t status, uint8_t pitch, uint8_t velocity, bool fromOther = false)
		{
			gnubase::audio::MidiMessage message;
			message.size = 3;
			message.bytes = {status, pitch, velocity};
			constexpr gnubase::HostTime second = 1000000000;
			(fromOther ? other : keys)->send(message, gnubase::hostTime() - second);
			return rig.played();
		}

		// Renders until the engine got what the remote keys did and the condition holds.
		bool until(const std::function<bool()>& condition)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (std::chrono::steady_clock::now() < deadline)
			{
				run(rig.client, "audio.render", {{"frames", 64}});
				if (condition())
					return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return false;
		}

		gnubase::midi::DummyBackend* backend = nullptr;
		Rig rig;
		std::unique_ptr<gnubase::midi::Output> keys;
		std::unique_ptr<gnubase::midi::Output> other;
	};
}

TEST_CASE(
	"remote keys light the pads, change them with the chord modifiers and come from the chosen input", "[integration]")
{
	// Reference manual pp. 1355, 1386-1387.
	RemoteRig remote;
	Client& client = remote.rig.client;
	CHECK(remote.send(0x90, 36, 90).size() == 4);
	const Json lit = waitFor(client, "chordPads.triggered");
	REQUIRE(lit.is_object());
	CHECK(lit.at("pad") == 0);
	CHECK(lit.at("on") == true);
	CHECK(lit.at("remote") == true);
	CHECK(remote.send(0x80, 36, 0).size() == 4);
	CHECK(waitFor(client, "chordPads.triggered").at("on") == false);

	// The chord modifiers, once activated: a key above the pads' range transposes the pad last triggered.
	const Json pads = run(client, "chordPads.get");
	CHECK(pads.at("modifiers") == false);
	CHECK(pads.at("modifierKeys").at("transposeUp") == 53);
	CHECK(remote.send(0x90, 53, 90) == std::vector<std::vector<int>>{{0x90, 53, 90}}); // off: it plays itself
	run(client, "chordPads.set", {{"modifiers", true}, {"modifierKeys", {{"transposeUp", 55}}}});
	CHECK(run(client, "chordPads.get").at("modifierKeys").at("transposeUp") == 55);
	// It plays the pad it changed while it is down, the pad's key being up.
	CHECK(remote.send(0x90, 55, 90).empty());
	CHECK(remote.until([&] { return pad(client, 0).at("chord") == "C#"; }));
	CHECK(remote.until([&] { return remote.rig.played().size() == 4; }));
	size_t offs = remote.send(0x80, 55, 0).size(); // the pad's note-offs
	CHECK(remote.until(
		[&]
		{
			offs += remote.rig.played().size();
			return offs == 4;
		}));
	CHECK_THAT(
		reject(client, "chordPads.set", {{"modifierKeys", {{"transposeUp", 128}}}}), ContainsSubstring("modifierKeys"));

	// Transpose All Pads by a controller, Modify All Pad Voicings by pitch bend (p. 1387): across their range, as one
	// undo step per gesture.
	run(client, "chordPads.set",
		{{"controlSources", {"pitchBend", "none", "controller"}}, {"controlNumbers", {1, 1, 74}}});
	run(client, "audio.render", {{"frames", 64}});
	const auto steps = run(client, "history.get").at("position").get<int64_t>();
	const auto control = [&](uint8_t status, uint8_t number, uint8_t value)
	{
		gnubase::audio::MidiMessage message;
		message.size = 3;
		message.bytes = {status, number, value};
		constexpr gnubase::HostTime second = 1000000000;
		remote.keys->send(message, gnubase::hostTime() - second);
		return remote.rig.played();
	};
	CHECK(control(0xB0, 74, 127).empty());
	CHECK(remote.until([&] { return pad(client, 1).at("chord") == "G#m"; })); // Dm 6 semitones up
	control(0xB0, 74, 117);
	CHECK(remote.until([&] { return pad(client, 1).at("chord") == "Gm"; }));
	CHECK(run(client, "history.get").at("position").get<int64_t>() == steps + 1);
	CHECK(control(0xE0, 0, 127).empty());
	CHECK(remote.until([&] { return pad(client, 1).at("voicing") == 3; }));
	CHECK(control(0xB0, 7, 100) == std::vector<std::vector<int>>{}); // another controller plays on: no note

	// Only the chosen MIDI input triggers the pads; the others' notes play themselves.
	run(client, "chordPads.set", {{"remotePort", "dummy/in/Other"}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(remote.send(0x90, 38, 90) == std::vector<std::vector<int>>{{0x90, 38, 90}});
	CHECK(remote.send(0x90, 38, 90, true).size() == 4);
}

TEST_CASE("Assign from MIDI Input and MIDI Learn take the next notes from a MIDI input", "[integration]")
{
	// Reference manual pp. 1360, 1386-1388.
	RemoteRig remote;
	Client& client = remote.rig.client;
	run(client, "chordPads.listen", {{"assign", 3}});
	CHECK(run(client, "chordPads.get").at("listening") == Json{{"assign", 3}});
	// The notes play as they are, and once they are all released their chord goes onto the pad.
	CHECK(remote.send(0x90, 62, 90).size() == 1);
	remote.send(0x90, 65, 90);
	remote.send(0x90, 69, 90);
	remote.send(0x80, 62, 0);
	CHECK(pad(client, 3).at("chord") == "F");
	remote.send(0x80, 65, 0);
	remote.send(0x80, 69, 0);
	CHECK(remote.until([&] { return pad(client, 3).at("chord") == "Dm"; }));
	CHECK(remote.until([&] { return run(client, "chordPads.get").at("listening").is_null(); }));
	// The remote keys trigger the pads again.
	CHECK(remote.send(0x90, 36, 90).size() == 4);
	remote.send(0x80, 36, 0);

	// MIDI Learn: two keys make the pads' remote range, one a modifier's key.
	run(client, "chordPads.listen", {{"learn", "range"}});
	remote.send(0x90, 59, 90);
	remote.send(0x90, 48, 90);
	CHECK(remote.until([&] { return run(client, "chordPads.get").at("remoteStart") == 48; }));
	CHECK(run(client, "chordPads.get").at("remoteEnd") == 59);
	run(client, "chordPads.listen", {{"learn", "nextVoicing"}});
	remote.send(0x90, 72, 90);
	CHECK(remote.until([&] { return run(client, "chordPads.get").at("modifierKeys").at("nextVoicing") == 72; }));
	// And one that ends without a note.
	run(client, "chordPads.listen", {{"learn", "range"}});
	run(client, "chordPads.listen");
	CHECK(run(client, "chordPads.get").at("listening").is_null());
	CHECK_THAT(reject(client, "chordPads.listen", {{"assign", 40}}), ContainsSubstring("no chord pad"));
}

TEST_CASE("pads triggered while recording become chord events on the record-enabled chord track", "[integration]")
{
	// Reference manual p. 1368.
	RemoteRig remote;
	Client& client = remote.rig.client;
	run(client, "track.add", {{"type", "chord"}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 4800}});
	run(client, "chordPads.trigger", {{"pad", 3}});
	run(client, "chordPads.trigger", {{"pad", 3}, {"on", false}});
	CHECK(run(client, "chord.get").at("events").empty()); // the chord track isn't record-enabled
	run(client, "chord.setTrack", {{"recordEnable", true}});
	run(client, "chordPads.trigger", {{"pad", 3}});
	run(client, "chordPads.trigger", {{"pad", 3}, {"on", false}});
	Json events = run(client, "chord.get").at("events");
	REQUIRE(events.size() == 1);
	CHECK(events[0].at("chord") == "F");
	CHECK(events[0].at("position") > 0);
	// From the remote keys too.
	run(client, "audio.render", {{"frames", 48000}});
	remote.send(0x90, 40, 90);
	CHECK(remote.until([&] { return run(client, "chord.get").at("events").size() == 2; }));
	CHECK(run(client, "chord.get").at("events")[1].at("chord") == "G");
	remote.send(0x80, 40, 0);
	run(client, "transport.stop");
}

TEST_CASE("Chord Pad Output Mode and the Chord Pads MIDI input route the pads' chords", "[integration]")
{
	// Reference manual pp. 1347, 1366-1367: on, the pads play into every track that monitors or is record-enabled;
	// off, only into those whose input routing is the chord pads, which take nothing else, and record them.
	Rig rig;
	Client& client = rig.client;
	CHECK(run(client, "chordPads.get").at("outputMode") == true);
	run(client, "chordPads.trigger", {{"pad", 0}});
	run(client, "chordPads.trigger", {{"pad", 0}, {"on", false}});
	CHECK(rig.played().size() == 8);
	run(client, "chordPads.set", {{"outputMode", false}});
	run(client, "audio.render", {{"frames", 64}});
	run(client, "chordPads.trigger", {{"pad", 0}});
	run(client, "chordPads.trigger", {{"pad", 0}, {"on", false}});
	CHECK(rig.played().empty());
	run(client, "channel.set", {{"channel", rig.channel}, {"midiInput", "chordPads"}});
	run(client, "audio.render", {{"frames", 64}});
	run(client, "chordPads.trigger", {{"pad", 0}});
	CHECK(rig.played().size() == 4);
	run(client, "chordPads.trigger", {{"pad", 0}, {"on", false}});
	CHECK(rig.played().size() == 4);
	run(client, "midi.keyboard", {{"messages", {{{"bytes", {0x90, 60, 100}}}}}});
	CHECK(rig.played().empty());
	// On, they play into it as well.
	run(client, "chordPads.set", {{"outputMode", true}});
	run(client, "audio.render", {{"frames", 64}});
	run(client, "chordPads.trigger", {{"pad", 0}});
	CHECK(rig.played().size() == 4);
	run(client, "chordPads.trigger", {{"pad", 0}, {"on", false}});
	CHECK(rig.played().size() == 4);
	CHECK_THAT(reject(client, "chordPads.set", {{"overlaps", "sometimes"}}), ContainsSubstring("overlaps"));
}

TEST_CASE("several players on several tracks: each track plays its player's voicings", "[integration]")
{
	// Reference manual p. 1377 (#1089): the track of a player gets its voicings, the others the selected player's.
	Rig rig;
	Client& client = rig.client;
	const Json added = run(client, "track.add", {{"type", "instrument"}});
	const auto pianoTrack = added.at("track").at("id").get<ObjectId>();
	const auto pianoChannel = added.at("channel").at("id").get<ObjectId>();
	run(client, "channel.set", {{"channel", pianoChannel}, {"instrument", "noteProbe"}, {"monitor", true}});
	run(client, "chordPads.addPlayer", {{"library", "piano"}, {"style", "fourNote"}, {"track", pianoTrack}});
	run(client, "chordPads.set", {{"player", 0}});
	run(client, "audio.render", {{"frames", 64}});
	run(client, "midi.capture", {{"start", true}});
	// The pitches each channel's instrument played.
	const auto played = [&]
	{
		run(client, "audio.render", {{"frames", 64}});
		std::map<ObjectId, std::vector<int>> pitches;
		const Json capture = run(client, "midi.capture");
		for (const Json& captured : capture.at("captured"))
			if ((captured.at("bytes").at(0).get<int>() & 0xF0) == 0x90)
				pitches[captured.at("channel").get<ObjectId>()].push_back(captured.at("bytes").at(1).get<int>());
		for (auto& [channel, notes] : pitches)
			std::sort(notes.begin(), notes.end());
		return pitches;
	};
	midi::VoicingStyle piano;
	piano.library = "piano";
	piano.style = "fourNote";
	std::vector<int> fourNote = midi::voiceChord(chord("C"), {0, false}, {}, piano);
	std::sort(fourNote.begin(), fourNote.end());
	const Json result = run(client, "chordPads.trigger", {{"pad", 0}});
	CHECK(result.at("pitches") == std::vector<int>{48, 60, 64, 67}); // the selected player's
	std::map<ObjectId, std::vector<int>> pitches = played();
	CHECK(pitches[rig.channel] == std::vector<int>{48, 60, 64, 67});
	CHECK(pitches[pianoChannel] == fourNote);
	run(client, "chordPads.trigger", {{"pad", 0}, {"on", false}});
	played();

	// One player a track, and only MIDI and instrument tracks.
	const Json players = run(client, "chordPads.get").at("players");
	CHECK(players.at(1).at("track") == pianoTrack);
	CHECK_THAT(reject(client, "chordPads.setPlayer", {{"player", 0}, {"track", pianoTrack}}),
		ContainsSubstring("another chord pads player"));
	CHECK_THAT(reject(client, "chordPads.setPlayer", {{"track", 9999}}), ContainsSubstring("MIDI or instrument track"));
	// Without a track the player plays only while it is selected.
	run(client, "chordPads.setPlayer", {{"player", 1}, {"track", 0}});
	run(client, "audio.render", {{"frames", 64}});
	run(client, "chordPads.trigger", {{"pad", 0}});
	pitches = played();
	CHECK(pitches[rig.channel] == std::vector<int>{48, 60, 64, 67});
	CHECK(pitches[pianoChannel] == std::vector<int>{48, 60, 64, 67});
	run(client, "chordPads.trigger", {{"pad", 0}, {"on", false}});
}

TEST_CASE("the pattern player mode plays a MIDI part's pattern with the notes of the chord that plays", "[integration]")
{
	// Reference manual pp. 1378-1380 (#1089): at 120 BPM and 48 kHz a quarter note is 24000 samples.
	Rig rig;
	Client& client = rig.client;
	const auto midiTrack = run(client, "track.add", {{"type", "midi"}}).at("track").at("id").get<ObjectId>();
	const auto part = run(client, "midiPart.add", {{"track", midiTrack}, {"position", 0}, {"length", 2 * g_beatTicks}})
						  .at("part")
						  .at("id")
						  .get<ObjectId>();
	// Four voices, one per eighth note: the lowest pitch is the bass, the highest the top voice.
	Json notes = Json::array();
	for (int i = 0; i < 4; ++i)
		notes.push_back({{"position", i * g_beatTicks / 2}, {"length", g_beatTicks / 4}, {"pitch", 40 + (3 * i)},
			{"velocity", 0.5}});
	run(client, "note.add", {{"part", part}, {"notes", notes}});
	CHECK_THAT(reject(client, "chordPads.setPattern", {{"part", 9999}}), ContainsSubstring("no MIDI part"));
	run(client, "chordPads.setPattern", {{"part", part}});
	Json pads = run(client, "chordPads.get");
	CHECK(pads.at("mode") == "pattern");
	CHECK(pads.at("pattern").at("voices") == 4);
	CHECK(pads.at("pattern").at("length") == 2 * g_beatTicks);
	REQUIRE(pads.at("pattern").at("notes").size() == 4);

	// C plays 48 60 64 67 as voices 0 to 3: the bass at once, an eighth note (12000 samples) apart.
	const auto ons = [](const std::vector<std::vector<int>>& played)
	{
		std::vector<int> pitches;
		for (const std::vector<int>& note : played)
			if ((note[0] & 0xF0) == 0x90)
				pitches.push_back(note[1]);
		return pitches;
	};
	// The windows end between the notes, which come a block or two after the trigger, as MIDI input does.
	run(client, "chordPads.trigger", {{"pad", 0}});
	run(client, "audio.render", {{"frames", 1000}});
	CHECK(ons(rig.played()) == std::vector<int>{48});
	run(client, "audio.render", {{"frames", 16872}}); // up to 18000
	CHECK(ons(rig.played()) == std::vector<int>{60});
	run(client, "audio.render", {{"frames", 11936}}); // 30000
	CHECK(ons(rig.played()) == std::vector<int>{64});
	run(client, "audio.render", {{"frames", 11936}}); // 42000
	CHECK(ons(rig.played()) == std::vector<int>{67});
	// Two beats later it starts over.
	run(client, "audio.render", {{"frames", 11936}}); // 54000
	CHECK(ons(rig.played()) == std::vector<int>{48});
	// Releasing the pad ends it.
	run(client, "chordPads.trigger", {{"pad", 0}, {"on", false}});
	rig.played();
	run(client, "audio.render", {{"frames", 48000}});
	CHECK(ons(rig.played()).empty());

	// A preset; plain chords again play the chord.
	run(client, "chordPads.setPattern", {{"preset", "brokenChord"}});
	CHECK(run(client, "chordPads.get").at("pattern").at("name") == "brokenChord");
	run(client, "chordPads.set", {{"mode", "plainChords"}});
	run(client, "audio.render", {{"frames", 64}});
	run(client, "chordPads.trigger", {{"pad", 0}});
	CHECK(ons(rig.played()).size() == 4);
	run(client, "chordPads.trigger", {{"pad", 0}, {"on", false}});
	// Import from MIDI Loop: a MIDI file's notes (p. 1379).
	const gnubase::test::TempDir dir;
	const std::string loop = (dir.path() / "Loop.mid").u8string();
	midifile::File file;
	file.ticksPerQuarter = 480;
	midifile::Track track;
	for (int i = 0; i < 3; ++i)
	{
		const int64_t tick = int64_t{i} * 480;
		track.events.push_back(midifile::channelEvent(tick, 0x90, static_cast<uint8_t>(50 + i), 100));
		track.events.push_back(midifile::channelEvent(tick + 240, 0x80, static_cast<uint8_t>(50 + i), 0));
	}
	track.end = int64_t{4} * 480;
	file.tracks.push_back(track);
	midifile::save(file, loop);
	run(client, "chordPads.setPattern", {{"file", loop}});
	Json imported = run(client, "chordPads.get").at("pattern");
	CHECK(imported.at("name") == "Loop");
	CHECK(imported.at("voices") == 3);
	CHECK(imported.at("length") == 4 * g_beatTicks);
	CHECK(imported.at("notes")[1].at("position") == g_beatTicks);
	CHECK(imported.at("notes")[1].at("length") == g_beatTicks / 2);
	CHECK_THAT(reject(client, "chordPads.setPattern", {{"file", (dir.path() / "None.mid").u8string()}}),
		ContainsSubstring("can't read the MIDI loop"));
	// A part of fewer than three voices is no pattern.
	const auto two = run(client, "midiPart.add", {{"track", midiTrack}, {"position", 0}, {"length", g_beatTicks}})
						 .at("part")
						 .at("id")
						 .get<ObjectId>();
	run(client, "note.add",
		{{"part", two},
			{"notes",
				{{{"position", 0}, {"length", 100}, {"pitch", 40}, {"velocity", 0.5}},
					{{"position", 100}, {"length", 100}, {"pitch", 41}, {"velocity", 0.5}}}}});
	CHECK_THAT(reject(client, "chordPads.setPattern", {{"part", two}}), ContainsSubstring("3 to 5 voices"));
}

TEST_CASE("the sections player mode plays a pad's chord section by section from the section keys", "[integration]")
{
	// Reference manual pp. 1381-1385, 1389 (#1089): C plays 48 60 64 67, the sections from the lowest note up.
	RemoteRig remote;
	Client& client = remote.rig.client;
	run(client, "chordPads.set", {{"mode", "sections"}});
	CHECK(run(client, "chordPads.get").at("sectionKeys") == std::vector<int>{55, 56, 57, 58, 59});
	run(client, "audio.render", {{"frames", 64}});
	// The pad's key alone plays nothing; with it down the section keys play its sections.
	CHECK(remote.send(0x90, 36, 90).empty());
	CHECK(remote.send(0x90, 55, 80) == std::vector<std::vector<int>>{{0x90, 48, 80}});
	CHECK(remote.send(0x90, 57, 80) == std::vector<std::vector<int>>{{0x90, 64, 80}});
	CHECK(remote.send(0x80, 57, 0) == std::vector<std::vector<int>>{{0x80, 64, 0}});
	// The pad's key going up ends them.
	CHECK(remote.send(0x80, 36, 0) == std::vector<std::vector<int>>{{0x80, 48, 0}});
	CHECK(remote.send(0x80, 55, 0).empty());
	// Without the pad's key down, sections play nothing; chordPads takes the pad triggered last.
	CHECK(remote.send(0x90, 56, 80).empty());
	remote.send(0x80, 56, 0);
	run(client, "chordPads.set", {{"sectionPlay", "chordPads"}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(remote.send(0x90, 56, 80) == std::vector<std::vector<int>>{{0x90, 60, 80}});
	CHECK(remote.send(0x80, 56, 0) == std::vector<std::vector<int>>{{0x80, 60, 0}});
	// A muted section plays nothing; a subsection is its section moved.
	run(client, "chordPads.set",
		{{"mutedSections", {false, true}}, {"subsectionKeys", {62}}, {"subsectionSections", {3}},
			{"subsectionOffsets", {12}}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(remote.send(0x90, 56, 80).empty());
	remote.send(0x80, 56, 0);
	CHECK(remote.send(0x90, 62, 80) == std::vector<std::vector<int>>{{0x90, 79, 80}});
	remote.send(0x80, 62, 0);
	CHECK_THAT(reject(client, "chordPads.set", {{"sectionKeys", {128}}}), ContainsSubstring("sectionKeys"));
}

TEST_CASE("a remote key's chord held or latched before the sections player mode ends with the key", "[integration]")
{
	RemoteRig remote;
	Client& client = remote.rig.client;
	const std::vector<std::vector<int>> offs = sorted({{0x83, 48, 0}, {0x82, 60, 0}, {0x81, 64, 0}, {0x80, 67, 0}});
	CHECK(remote.send(0x90, 36, 90).size() == 4);
	run(client, "chordPads.set", {{"mode", "sections"}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(sorted(remote.send(0x80, 36, 0)) == offs);
	// Latched: the key's next press selects the pad for the sections, its off ends the chord.
	run(client, "chordPads.set", {{"mode", "plainChords"}, {"latch", true}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(remote.send(0x90, 36, 90).size() == 4);
	CHECK(remote.send(0x80, 36, 0).empty());
	run(client, "chordPads.set", {{"mode", "sections"}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(remote.send(0x90, 36, 90).empty());
	CHECK(sorted(remote.send(0x80, 36, 0)) == offs);
}

TEST_CASE("a remote key's latched chord ends with its next press when the key plays no pad any more", "[integration]")
{
	RemoteRig remote;
	Client& client = remote.rig.client;
	const std::vector<std::vector<int>> offs = sorted({{0x83, 48, 0}, {0x82, 60, 0}, {0x81, 64, 0}, {0x80, 67, 0}});
	run(client, "chordPads.set", {{"latch", true}});
	run(client, "audio.render", {{"frames", 64}});
	// The remote range moves away from the key that latched the chord.
	CHECK(remote.send(0x90, 36, 90).size() == 4);
	CHECK(remote.send(0x80, 36, 0).empty());
	run(client, "chordPads.set", {{"remoteStart", 60}, {"remoteEnd", 71}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(sorted(remote.send(0x90, 36, 90)) == offs);
	// The key holds nothing now, so its off goes through as it would without the pads.
	CHECK(remote.send(0x80, 36, 0) == std::vector<std::vector<int>>{{0x80, 36, 0}});
	// A section key held in the sections mode latches its section once the player mode changes.
	run(client, "chordPads.set", {{"remoteStart", 36}, {"remoteEnd", 47}, {"mode", "sections"}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(remote.send(0x90, 36, 90).empty());
	CHECK(remote.send(0x90, 55, 80) == std::vector<std::vector<int>>{{0x90, 48, 80}});
	run(client, "chordPads.set", {{"mode", "plainChords"}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(remote.send(0x80, 55, 0).empty());
	CHECK(remote.send(0x80, 36, 0) == std::vector<std::vector<int>>{{0x80, 36, 0}});
	CHECK(remote.send(0x90, 55, 80) == std::vector<std::vector<int>>{{0x80, 48, 0}});
}

TEST_CASE("Select/Mute Player's remote keys select the previous and next player and mute it", "[integration]")
{
	// Reference manual p. 1389.
	RemoteRig remote;
	Client& client = remote.rig.client;
	run(client, "chordPads.addPlayer", {{"library", "piano"}});
	run(client, "chordPads.set", {{"player", 0}, {"playerKeys", {70, 71, 72}}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(remote.send(0x90, 71, 90).empty());
	remote.send(0x80, 71, 0);
	CHECK(remote.until([&] { return run(client, "chordPads.get").at("player") == 1; }));
	remote.send(0x90, 70, 90);
	CHECK(remote.until([&] { return run(client, "chordPads.get").at("player") == 0; }));
	remote.send(0x90, 72, 90);
	CHECK(remote.until([&] { return run(client, "chordPads.get").at("players")[0].at("muted") == true; }));
	// A muted player plays nothing.
	run(client, "audio.render", {{"frames", 64}});
	CHECK(remote.send(0x90, 36, 90).empty());
}

TEST_CASE("the pattern player repeats its pattern on the held chord's voices", "[unit]")
{
	// midi::PadPattern on its own: four eighth notes on four voices, two beats long, at 120 BPM and 48 kHz.
	midi::PadPatternSteps steps;
	steps.on = true;
	steps.length = 2 * g_beatTicks;
	for (int i = 0; i < 4; ++i)
		steps.steps.at(steps.count++) = {i * g_beatTicks / 2, g_beatTicks / 4, static_cast<uint8_t>(i), 100};
	midi::PadPattern pattern;
	gnubase::audio::MidiBuffer pads(256);
	for (const uint8_t pitch : std::array<uint8_t, 4>{48, 60, 64, 67})
	{
		gnubase::audio::MidiMessage on;
		on.size = 3;
		on.bytes = {0x90, pitch, 90};
		pads.add(on);
	}
	const double ticksPerFrame = 2.0 * static_cast<double>(g_beatTicks) / 48000.0;
	std::vector<std::pair<int64_t, int>> ons;
	for (int64_t block = 0; block < 800; ++block)
	{
		pattern.process(pads, steps, 64, ticksPerFrame);
		for (const gnubase::audio::MidiMessage& message : pads)
			if ((message.bytes[0] & 0xF0) == 0x90)
				ons.emplace_back((block * 64) + message.offset, message.bytes[1]);
		pads.clear();
	}
	REQUIRE(ons.size() >= 5);
	CHECK(ons[0] == std::pair<int64_t, int>{0, 48});
	CHECK(ons[1].second == 60);
	CHECK(std::abs(ons[1].first - 12000) <= 1);
	CHECK(ons[3].second == 67);
	CHECK(ons[4].second == 48);
	CHECK(std::abs(ons[4].first - 48000) <= 1);
}

TEST_CASE("the pattern player passes on the offs of notes it didn't start", "[unit]")
{
	midi::PadPatternSteps steps;
	steps.on = true;
	steps.length = g_beatTicks;
	steps.steps.at(steps.count++) = {0, g_beatTicks / 4, 0, 100};
	midi::PadPattern pattern;
	gnubase::audio::MidiBuffer pads(16);
	gnubase::audio::MidiMessage off;
	off.size = 3;
	off.bytes = {0x82, 60, 0};
	off.offset = 5;
	pads.add(off);
	pattern.process(pads, steps, 64, 1.0);
	REQUIRE(pads.size() == 1);
	CHECK(pads.begin()->bytes == off.bytes);
	CHECK(pads.begin()->offset == 5);
}

TEST_CASE("chords held or latched while the player mode changes to pattern end on the track", "[integration]")
{
	Rig rig;
	Client& client = rig.client;
	const std::vector<std::vector<int>> offs = sorted({{0x83, 48, 0}, {0x82, 60, 0}, {0x81, 64, 0}, {0x80, 67, 0}});
	run(client, "chordPads.trigger", {{"pad", 0}});
	CHECK(rig.played().size() == 4);
	run(client, "chordPads.setPattern", {{"preset", "brokenChord"}});
	run(client, "audio.render", {{"frames", 64}});
	run(client, "chordPads.trigger", {{"pad", 0}, {"on", false}});
	CHECK(sorted(rig.played()) == offs);
	// Latched, the pad's next trigger ends it.
	run(client, "chordPads.set", {{"mode", "plainChords"}, {"latch", true}});
	run(client, "audio.render", {{"frames", 64}});
	run(client, "chordPads.trigger", {{"pad", 0}});
	CHECK(rig.played().size() == 4);
	run(client, "chordPads.set", {{"mode", "pattern"}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(run(client, "chordPads.trigger", {{"pad", 0}}).at("on") == false);
	CHECK(sorted(rig.played()) == offs);
}

TEST_CASE("with Snap Playback a note-off in the block that snaps its note-on comes after it", "[unit]")
{
	// A note-on and its off in one block whose next grid position is at frame 40.
	midi::PadSnap snap;
	gnubase::audio::MidiBuffer pads(16);
	gnubase::audio::MidiMessage note;
	note.size = 3;
	note.bytes = {0x91, 60, 90};
	note.offset = 2;
	pads.add(note);
	note.bytes = {0x81, 60, 0};
	note.offset = 3;
	pads.add(note);
	snap.process(pads, true, true, 1000, 64, [](int64_t) { return int64_t{1040}; });
	std::vector<std::pair<int, uint32_t>> out;
	for (const gnubase::audio::MidiMessage& message : pads)
		out.emplace_back(message.bytes[0], message.offset);
	CHECK(out == std::vector<std::pair<int, uint32_t>>{{0x91, 40}, {0x81, 40}});
}

TEST_CASE("with Snap Playback to Musical Grid a pad's chord starts at the next grid position", "[integration]")
{
	// Reference manual p. 1349: a quarter note grid at 120 BPM is 24000 samples at 48 kHz.
	Rig rig;
	Client& client = rig.client;
	run(client, "quantize.set", {{"preset", "1/4"}});
	run(client, "chordPads.set", {{"snapPlayback", true}});
	CHECK(run(client, "chordPads.get").at("snapPlayback") == true);
	// While the transport stands it plays at once.
	run(client, "chordPads.trigger", {{"pad", 0}});
	CHECK(rig.played().size() == 4);
	run(client, "chordPads.trigger", {{"pad", 0}, {"on", false}});
	CHECK(rig.played().size() == 4);
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 4800}});
	rig.played();
	run(client, "chordPads.trigger", {{"pad", 5}});
	CHECK(rig.played().empty()); // a tenth of a second into the beat
	run(client, "audio.render", {{"frames", 12000}});
	CHECK(rig.played().empty());
	run(client, "audio.render", {{"frames", 7104}}); // up to just past the second beat
	CHECK(rig.played().size() == 4);
	// An off before the grid position ends the chord right after it starts.
	run(client, "chordPads.trigger", {{"pad", 5}, {"on", false}});
	run(client, "chordPads.trigger", {{"pad", 4}});
	run(client, "chordPads.trigger", {{"pad", 4}, {"on", false}});
	CHECK(rig.played().size() == 4); // the offs of the Am chord
	run(client, "audio.render", {{"frames", 24000}});
	CHECK(rig.played().size() == 8); // G's ons and offs
	run(client, "transport.stop");
}

TEST_CASE(
	"a chord played over another holds, ties or stops the first one's notes, and latched pads toggle", "[integration]")
{
	// Reference manual pp. 1379, 1386: C (48 60 64 67) and Em (52 64 67 71) share E3 and G3.
	Rig rig;
	Client& client = rig.client;
	const auto trigger = [&](int pad, bool on)
	{
		run(client, "chordPads.trigger", {{"pad", pad}, {"on", on}});
		return sorted(rig.played());
	};
	CHECK(run(client, "chordPads.get").at("overlaps") == "holdFirst");
	// Hold First: the common notes go on sounding, the first chord's others until its off.
	CHECK(trigger(0, true).size() == 4);
	CHECK(trigger(2, true) == sorted({{0x93, 52, 100}, {0x90, 71, 100}}));
	CHECK(trigger(0, false) == sorted({{0x83, 48, 0}, {0x82, 60, 0}}));
	CHECK(trigger(2, false) == sorted({{0x83, 52, 0}, {0x81, 64, 0}, {0x80, 67, 0}, {0x80, 71, 0}}));
	// Legato: the first chord's other notes end, the common ones go on.
	run(client, "chordPads.set", {{"overlaps", "legato"}});
	CHECK(trigger(0, true).size() == 4);
	CHECK(trigger(2, true) == sorted({{0x93, 52, 100}, {0x90, 71, 100}, {0x83, 48, 0}, {0x82, 60, 0}}));
	CHECK(trigger(0, false).empty());
	CHECK(trigger(2, false).size() == 4);
	// Stop First: all of the first chord's notes end, and the second's all start.
	run(client, "chordPads.set", {{"overlaps", "stopFirst"}});
	CHECK(trigger(0, true).size() == 4);
	CHECK(trigger(2, true).size() == 8);
	CHECK(trigger(0, false).empty());
	CHECK(trigger(2, false).size() == 4);
	// Latch Chords: an off ends nothing, the pad's next trigger ends its chord.
	run(client, "chordPads.set", {{"latch", true}});
	CHECK(trigger(0, true).size() == 4);
	CHECK(trigger(0, false).empty());
	CHECK(run(client, "chordPads.trigger", {{"pad", 0}}).at("on") == false);
	CHECK(rig.played().size() == 4);
	CHECK(trigger(0, true).size() == 4);
	CHECK(trigger(2, true).size() == 8); // Stop First ends the latched chord
	CHECK(trigger(2, true).size() == 4);
}

TEST_CASE("step input and pads dragged onto tracks make chord events and MIDI parts", "[unit]")
{
	Engine engine;
	Client client(engine);
	CHECK_THAT(reject(client, "chordPads.step", {{"pad", 0}}), ContainsSubstring("step input is off"));
	run(client, "chordPads.set", {{"stepInput", true}, {"stepPosition", g_bar}});
	run(client, "chordPads.step", {{"pad", 0}});
	run(client, "chordPads.step", {{"pad", 5}});
	Json chords = run(client, "chord.get");
	REQUIRE(chords.at("events").size() == 2);
	CHECK(chords.at("events")[0].at("chord") == "C");
	CHECK(chords.at("events")[0].at("position") == g_bar);
	CHECK(chords.at("events")[1].at("chord") == "Am");
	CHECK(chords.at("events")[1].at("position") == 2 * g_bar);
	CHECK(run(client, "chordPads.get").at("stepPosition") == 3 * g_bar);
	// Left and Right move the step position by bars; a pad's chord goes where a chord already is in its place, and
	// at the start of the bar of a position within one (#967).
	run(client, "chordPads.set", {{"stepBars", -1}});
	CHECK(run(client, "chordPads.get").at("stepPosition") == 2 * g_bar);
	run(client, "chordPads.step", {{"pad", 3}});
	chords = run(client, "chord.get");
	REQUIRE(chords.at("events").size() == 2);
	CHECK(chords.at("events")[1].at("chord") == "F");
	CHECK(chords.at("events")[1].at("position") == 2 * g_bar);
	run(client, "chordPads.set", {{"stepPosition", g_bar + g_beatTicks}});
	run(client, "chordPads.step", {{"pad", 5}});
	chords = run(client, "chord.get");
	REQUIRE(chords.at("events").size() == 2);
	CHECK(chords.at("events")[0].at("chord") == "Am");
	CHECK(chords.at("events")[0].at("position") == g_bar);
	CHECK(run(client, "chordPads.get").at("stepPosition") == 2 * g_bar);
	run(client, "chordPads.set", {{"stepBars", -5}});
	CHECK(run(client, "chordPads.get").at("stepPosition") == 0);
	run(client, "chordPads.set", {{"stepPosition", g_bar}});
	// Move Insert: the events from the position on move a bar later.
	run(client, "chordPads.set", {{"moveInsert", true}, {"stepPosition", g_bar}});
	run(client, "chordPads.step", {{"pad", 4}});
	chords = run(client, "chord.get");
	REQUIRE(chords.at("events").size() == 3);
	CHECK(chords.at("events")[0].at("chord") == "G");
	CHECK(chords.at("events")[1].at("position") == 2 * g_bar);
	CHECK(chords.at("events")[2].at("position") == 3 * g_bar);

	// Onto the chord track a chord event, onto a MIDI track a part a bar long with the pad's voicing.
	const auto chordTrack = chords.at("track").get<ObjectId>();
	run(client, "chordPads.toTrack", {{"pad", 3}, {"track", chordTrack}, {"position", 5 * g_bar}});
	CHECK(run(client, "chord.get").at("events").back().at("chord") == "F");
	const auto midiTrack = run(client, "track.add", {{"type", "midi"}}).at("track").at("id").get<ObjectId>();
	const Json part = run(client, "chordPads.toTrack", {{"pad", 0}, {"track", midiTrack}, {"position", 0}}).at("part");
	CHECK(part.at("length") == g_bar);
	std::vector<int> pitches;
	for (const Json& note : part.at("notes"))
		pitches.push_back(note.at("pitch").get<int>());
	std::sort(pitches.begin(), pitches.end());
	CHECK(pitches == std::vector<int>{48, 60, 64, 67});
}

TEST_CASE("step input finds and moves chord events on a chord track in linear time base", "[unit]")
{
	// At 121 BPM a bar's start lies between two samples: its ticks don't come back from the samples a linear chord
	// track keeps (#1048), so step input compares and moves the events in the track's time base.
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempo", 121.0}});
	run(client, "track.add", {{"type", "chord"}, {"timeBase", "linear"}});
	run(client, "chordPads.set", {{"stepInput", true}, {"stepPosition", g_bar}});
	run(client, "chordPads.step", {{"pad", 0}});
	const auto first = run(client, "chord.get").at("events")[0].at("position").get<int64_t>();
	run(client, "chordPads.set", {{"stepBars", -1}});
	run(client, "chordPads.step", {{"pad", 5}});
	Json chords = run(client, "chord.get");
	REQUIRE(chords.at("events").size() == 1);
	CHECK(chords.at("events")[0].at("chord") == "Am");
	CHECK(chords.at("events")[0].at("position") == first);
	const auto second = run(client, "chordPads.get").at("stepPosition").get<int64_t>();
	CHECK(second == 2 * g_bar);

	// Move Insert moves the event at the step position to the next bar's start, where the next step finds it.
	run(client, "chordPads.set", {{"moveInsert", true}, {"stepPosition", g_bar}});
	run(client, "chordPads.step", {{"pad", 3}});
	chords = run(client, "chord.get");
	REQUIRE(chords.at("events").size() == 2);
	CHECK(chords.at("events")[0].at("chord") == "F");
	CHECK(chords.at("events")[0].at("position") == first);
	const auto moved = chords.at("events")[1].at("position").get<int64_t>();
	run(client, "chordPads.step", {{"pad", 4}});
	chords = run(client, "chord.get");
	REQUIRE(chords.at("events").size() == 3);
	CHECK(chords.at("events")[1].at("chord") == "G");
	CHECK(chords.at("events")[1].at("position") == moved);
	CHECK(chords.at("events")[2].at("chord") == "Am");
	run(client, "chordPads.set", {{"moveInsert", false}, {"stepPosition", 2 * g_bar}});
	run(client, "chordPads.step", {{"pad", 0}});
	chords = run(client, "chord.get");
	REQUIRE(chords.at("events").size() == 3);
	CHECK(chords.at("events")[1].at("chord") == "C");
	CHECK(chords.at("events")[1].at("position") == moved);
}

TEST_CASE("Move Insert keeps chord events on bar starts across a tempo change on a linear chord track", "[unit]")
{
	// #1048: the events move a bar in ticks, those on a bar's start to the next bar's start in samples, wherever the
	// tempo changes after the step position.
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	run(client, "tempo.add", {{"position", 0}, {"bpm", 121.0}, {"ramp", false}});
	run(client, "tempo.add", {{"position", 3 * g_bar}, {"bpm", 60.0}, {"ramp", false}});
	run(client, "track.add", {{"type", "chord"}, {"timeBase", "linear"}});
	run(client, "chordPads.set", {{"stepInput", true}, {"stepPosition", 5 * g_bar}});
	run(client, "chordPads.step", {{"pad", 3}});
	const auto barFive = run(client, "chord.get").at("events")[0].at("position").get<int64_t>();
	run(client, "chordPads.set", {{"stepPosition", 4 * g_bar}});
	run(client, "chordPads.step", {{"pad", 0}});
	run(client, "chordPads.set", {{"moveInsert", true}, {"stepPosition", g_bar}});
	run(client, "chordPads.step", {{"pad", 5}});
	Json chords = run(client, "chord.get");
	REQUIRE(chords.at("events").size() == 3);
	CHECK(chords.at("events")[1].at("chord") == "C");
	CHECK(chords.at("events")[1].at("position") == barFive);
	run(client, "chordPads.set", {{"moveInsert", false}, {"stepPosition", 5 * g_bar}});
	run(client, "chordPads.step", {{"pad", 4}});
	chords = run(client, "chord.get");
	REQUIRE(chords.at("events").size() == 3);
	CHECK(chords.at("events")[1].at("chord") == "G");
	CHECK(chords.at("events")[2].at("chord") == "F");
}

TEST_CASE("the step position stays within the longest project, however far stepBars moves it", "[unit]")
{
	// #1048: stepBars of any size and stepPosition past the longest project's end (2^48 samples) don't overflow.
	Engine engine;
	Client client(engine);
	const auto velocity = run(client, "chordPads.get").at("velocity");
	CHECK_THAT(reject(client, "chordPads.set", {{"velocity", 50}, {"stepPosition", INT64_MAX}}),
		ContainsSubstring("stepPosition must be 0 to"));
	CHECK(run(client, "chordPads.get").at("velocity") == velocity); // a refused change changes nothing
	run(client, "chordPads.set", {{"stepInput", true}, {"stepBars", INT64_MAX}});
	const auto last = run(client, "chordPads.get").at("stepPosition").get<int64_t>();
	CHECK(last > 1000000 * g_bar);
	CHECK(last % g_bar == 0);
	run(client, "chordPads.set", {{"stepBars", 1}});
	CHECK(run(client, "chordPads.get").at("stepPosition") == last);
	run(client, "chordPads.set", {{"stepPosition", last - g_bar}, {"stepBars", INT64_MAX}});
	CHECK(run(client, "chordPads.get").at("stepPosition") == last);
	// More bars than a signed 64-bit number holds are as far on, not a bar back.
	run(client, "chordPads.set", {{"stepPosition", last - g_bar}, {"stepBars", UINT64_MAX}});
	CHECK(run(client, "chordPads.get").at("stepPosition") == last);
	// A step at the last bar writes its chord and leaves the position at the bar's end, which chordPads.set takes
	// back, and from where no step goes on.
	run(client, "chordPads.step", {{"pad", 0}});
	CHECK(run(client, "chord.get").at("events")[0].at("position") == last);
	CHECK(run(client, "chordPads.get").at("stepPosition") == last + g_bar);
	CHECK_THAT(reject(client, "chordPads.step", {{"pad", 0}}), ContainsSubstring("past the longest project's end"));
	run(client, "chordPads.set", {{"stepPosition", last + g_bar}});
	CHECK_THAT(reject(client, "chordPads.step", {{"pad", 0}}), ContainsSubstring("past the longest project's end"));
	CHECK_THAT(reject(client, "chordPads.set", {{"stepPosition", last + g_bar + 1}}),
		ContainsSubstring("stepPosition must be 0 to"));
	run(client, "chordPads.set", {{"stepBars", INT64_MIN}});
	CHECK(run(client, "chordPads.get").at("stepPosition") == 0);
	run(client, "chordPads.set", {{"stepPosition", 3 * g_bar}, {"stepBars", -2}});
	CHECK(run(client, "chordPads.get").at("stepPosition") == g_bar);
}

TEST_CASE("moving the step input position is no undo step, and undoing a step takes it back", "[unit]")
{
	// #1089: moving the step input cursor is navigation, like moving the project cursor.
	Engine engine;
	Client client(engine);
	const auto historyPosition = [&client] { return run(client, "history.get").at("position").get<int64_t>(); };
	const auto stepPosition = [&client] { return run(client, "chordPads.get").at("stepPosition").get<int64_t>(); };
	run(client, "chordPads.set", {{"stepInput", true}, {"moveInsert", true}});
	const int64_t steps = historyPosition();
	run(client, "chordPads.set", {{"stepBars", 3}});
	run(client, "chordPads.set", {{"stepBars", -1}});
	CHECK(stepPosition() == 2 * g_bar);
	run(client, "chordPads.set", {{"stepPosition", g_bar}});
	CHECK(historyPosition() == steps);
	// #1174: a negative position is refused.
	CHECK_THAT(reject(client, "chordPads.set", {{"stepPosition", -1}}), ContainsSubstring("stepPosition must be 0 to"));

	// #1174: one undo takes back a step that adds the chord track: the chord, the track and the step position.
	run(client, "chordPads.step", {{"pad", 0}});
	CHECK(historyPosition() == steps + 1);
	CHECK(run(client, "project.get").at("tracks").size() == 1);
	CHECK(stepPosition() == 2 * g_bar);
	run(client, "history.undo");
	CHECK(run(client, "chord.get").at("events").empty());
	CHECK(run(client, "project.get").at("tracks").empty());
	CHECK(stepPosition() == g_bar);

	// And a Move Insert step: the chord, the moved events and the step position; redo brings them back.
	run(client, "chord.add", {{"position", g_bar}, {"chord", "Dm"}});
	run(client, "chord.add", {{"position", 2 * g_bar}, {"chord", "G"}});
	run(client, "chordPads.step", {{"pad", 0}});
	Json chords = run(client, "chord.get");
	REQUIRE(chords.at("events").size() == 3);
	CHECK(chords.at("events")[0].at("chord") == "C");
	CHECK(chords.at("events")[2].at("position") == 3 * g_bar);
	CHECK(stepPosition() == 2 * g_bar);
	run(client, "history.undo");
	chords = run(client, "chord.get");
	REQUIRE(chords.at("events").size() == 2);
	CHECK(chords.at("events")[0].at("position") == g_bar);
	CHECK(chords.at("events")[1].at("position") == 2 * g_bar);
	CHECK(stepPosition() == g_bar);
	run(client, "history.redo");
	CHECK(run(client, "chord.get").at("events").size() == 3);
	CHECK(stepPosition() == 2 * g_bar);

	// Undoing an edit after the cursor moved leaves the cursor where it is.
	run(client, "chordPads.set", {{"stepBars", 4}});
	run(client, "chordPads.set", {{"velocity", 50}});
	run(client, "history.undo");
	CHECK(stepPosition() == 6 * g_bar);
	// The position only goes along with an edit when the edit's command moved it.
	run(client, "chordPads.set", {{"velocity", 60}, {"stepBars", -1}});
	CHECK(stepPosition() == 5 * g_bar);
	run(client, "history.undo");
	CHECK(stepPosition() == 6 * g_bar);
}

TEST_CASE("step input stays in range at very low sample rates and refuses to move far chord events", "[unit]")
{
	// #1174: at 100 Hz the longest project's ticks would leave int64_t; step input stops where it does at 8 kHz.
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"sampleRate", 100.0}});
	run(client, "chordPads.set", {{"stepInput", true}, {"stepBars", INT64_MAX}});
	const auto last = run(client, "chordPads.get").at("stepPosition").get<int64_t>();
	CHECK(last > 1000000 * g_bar);
	CHECK(last % g_bar == 0);
	run(client, "chordPads.set", {{"stepPosition", g_bar}});
	run(client, "chordPads.step", {{"pad", 0}});
	CHECK(run(client, "chord.get").at("events")[0].at("position") == g_bar);
	CHECK(run(client, "chordPads.get").at("stepPosition") == 2 * g_bar);
	Project project;
	project.sampleRate = 100.0;
	chordPadsFromJson(project, {{"stepPosition", INT64_MAX}});
	CHECK(project.chordPads.stepPosition == last + g_bar);
	project.sampleRate = 0.0;
	chordPadsFromJson(project, {{"stepPosition", INT64_MAX}});
	CHECK(project.chordPads.stepPosition == last + g_bar);

	// Move Insert refuses to move a chord event past the longest project's end a bar on, in musical and in linear time
	// base, and changes nothing.
	run(client, "chord.add", {{"position", INT64_MAX - 1}, {"chord", "F"}});
	run(client, "chordPads.set", {{"moveInsert", true}, {"stepPosition", 0}});
	CHECK_THAT(reject(client, "chordPads.step", {{"pad", 4}}), ContainsSubstring("past the longest project's end"));
	CHECK(run(client, "chord.get").at("events").size() == 2);
	CHECK(run(client, "chord.get").at("events")[1].at("position") == INT64_MAX - 1);
	Engine linearEngine;
	Client linear(linearEngine);
	run(linear, "track.add", {{"type", "chord"}, {"timeBase", "linear"}});
	run(linear, "chord.add", {{"position", INT64_MAX - 1}, {"chord", "F"}});
	run(linear, "chordPads.set", {{"stepInput", true}, {"moveInsert", true}});
	CHECK_THAT(reject(linear, "chordPads.step", {{"pad", 4}}), ContainsSubstring("past the longest project's end"));
	CHECK(run(linear, "chord.get").at("events").size() == 1);
}

TEST_CASE("chord pads from a file with no pads, a reference below -1 or a step position out of range keep sound values",
	"[unit]")
{
	Project project;
	chordPadsFromJson(project, {{"pads", Json::array()}, {"reference", -5}, {"stepPosition", INT64_MAX}});
	CHECK(project.chordPads.pads.size() == 12);
	CHECK(project.chordPads.reference == -1);
	// A step position past the end of the bar the longest project ends in comes back to it, where step input stops
	// (#1048).
	namespace gtime = gnubase::time;
	const gtime::Ticks longest =
		gtime::toTicks({gtime::TimeBase::Linear, gtime::g_maxPosition}, project.tempo, project.sampleRate);
	CHECK(
		project.chordPads.stepPosition == project.signatures.barStart(project.signatures.toBarsBeats(longest).bar + 1));
	chordPadsFromJson(project, {{"stepPosition", -1}});
	CHECK(project.chordPads.stepPosition == 0);
}

TEST_CASE("the chord pads are saved with the project", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string path = (dir.path() / "Pads.dawproject").u8string();
	Json saved;
	{
		Engine engine;
		Client client(engine);
		run(client, "chordPads.loadPreset", {{"preset", "lydian"}, {"root", 5}});
		run(client, "chordPads.set", {{"remote", true}, {"velocity", 64}, {"octaveOffset", -1}, {"padCount", 9}});
		run(client, "chordPads.modify", {{"pads", {1}}, {"voicing", -1}, {"tensions", 1}});
		run(client, "chordPads.setPad", {{"pad", 2}, {"locked", true}});
		run(client, "chordPads.setPad", {{"pad", 0}, {"reference", true}});
		saved = run(client, "chordPads.get");
		run(client, "project.save", {{"path", path}});
	}
	Engine engine;
	Client client(engine);
	run(client, "project.load", {{"path", path}});
	CHECK(run(client, "chordPads.get") == saved);
}
