// SPDX-License-Identifier: GPL-3.0-or-later
// The chord pads and the chord assistant (#174, reference manual pp. 1346-1391): the music theory of presets, degrees,
// tensions, inversions and suggestions, the chordPads.* commands with undo and the project file, pads that play and
// record, step input, pads dragged onto tracks and the pads' remote keys on a MIDI input.
#include "engine/document/chordpads.h"
#include "engine/engine.h"
#include "mcp/server.h"
#include "platform/audio/device.h"
#include "platform/hosttime.h"
#include "platform/midi/dummyport.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
