// SPDX-License-Identifier: GPL-3.0-or-later
// The chord track (#173, reference manual pp. 165, 1319-1345): chord and scale events, voicings, converting chord
// events to MIDI, MIDI tracks that follow the chord track over reference progressions, chord events from MIDI and the
// project file.
#include "engine/document/document.h"
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "platform/audio/device.h"
#include "platform/midi/dummyport.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using gnubase::test::readZip;
using gnubase::test::writeZip;

namespace
{
	constexpr int64_t g_beatTicks = 1720320;
	constexpr int64_t g_bar = 4 * g_beatTicks;           // 4/4
	constexpr int64_t g_barSamples = int64_t{4} * 24000; // at 120 BPM and 48 kHz

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

	using Bars = std::vector<std::vector<int>>;

	// A MIDI track with a part of four bars from 0.
	struct Song
	{
		Song()
		{
			const Json added = run(client, "track.add", {{"type", "midi"}});
			channel = added.at("channel").at("id").get<ObjectId>();
			track = added.at("track").at("id").get<ObjectId>();
			part = run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 4 * g_bar}})
					   .at("part")
					   .at("id")
					   .get<ObjectId>();
		}

		// Each bar's pitches as a chord a bar long, or arpeggiated, a note a beat.
		void play(const Bars& bars, bool arpeggio = false)
		{
			Json notes = Json::array();
			for (size_t bar = 0; bar < bars.size(); ++bar)
				for (size_t i = 0; i < bars[bar].size(); ++i)
				{
					const int64_t beat = arpeggio ? static_cast<int64_t>(i) : 0;
					notes.push_back({{"position", (static_cast<int64_t>(bar) * g_bar) + (beat * g_beatTicks)},
						{"length", arpeggio ? g_beatTicks : g_bar}, {"pitch", bars[bar][i]}, {"velocity", 0.75}});
				}
			run(client, "note.add", {{"part", part}, {"notes", notes}});
		}

		// A chord event at the start of each bar.
		void chords(const std::vector<std::string>& names)
		{
			for (size_t bar = 0; bar < names.size(); ++bar)
				run(client, "chord.add", {{"position", static_cast<int64_t>(bar) * g_bar}, {"chord", names[bar]}});
		}

		// The pitches the track plays in each bar, from the render snapshot, from the lowest up.
		Bars pitches()
		{
			Bars bars(4);
			for (const RenderChannel& render : engine.renderSnapshots().acquire().channels)
				if (render.id == channel)
					for (const RenderNote& note : render.notes)
					{
						const auto bar = static_cast<size_t>(note.start / g_barSamples);
						if (bar < bars.size())
							bars[bar].push_back(note.pitch);
					}
			for (std::vector<int>& bar : bars)
				std::sort(bar.begin(), bar.end());
			return bars;
		}

		Engine engine;
		Client client{engine};
		ObjectId channel = 0;
		ObjectId track = 0;
		ObjectId part = 0;
	};

	std::vector<std::string> chordNames(Client& client)
	{
		std::vector<std::string> names;
		const Json chord = run(client, "chord.get");
		for (const Json& event : chord.at("events"))
			names.push_back(event.at("chord").get<std::string>());
		return names;
	}

	// The I-vi-IV-V progression's triads in C, in root position from C3.
	Bars triads()
	{
		return {{60, 64, 67}, {57, 60, 64}, {65, 69, 72}, {55, 59, 62}};
	}
}

TEST_CASE("the chord track holds chord events and scale events", "[integration]")
{
	Song song;
	Client& client = song.client;
	CHECK(run(client, "chord.get").at("track") == 0);
	// The first event adds the chord track; the draw tool's events are undefined.
	const Json c = run(client, "chord.add", {{"position", 0}, {"chord", "C"}});
	const Json chord = run(client, "chord.get");
	CHECK(chord.at("track") != 0);
	CHECK(c.at("event").at("chord") == "C");
	CHECK(c.at("event").at("scale") == "C major");
	CHECK(c.at("event").at("pitches") == Json{48, 60, 64, 67});
	const Json x = run(client, "chord.add", {{"position", g_bar}});
	CHECK(x.at("event").at("chord") == "X");
	CHECK(x.at("event").at("pitches").empty());
	CHECK(reject(client, "chord.add", {{"position", g_bar}, {"chord", "D"}}).find("position") != std::string::npos);
	CHECK(reject(client, "chord.add", {{"position", 2 * g_bar}, {"chord", "H7"}}).find("H7") != std::string::npos);
	CHECK(reject(client, "track.add", {{"type", "chord"}}).find("one chord track") != std::string::npos);
	// Text input names chords as the chord editor does; events stay in timeline order.
	const auto xId = x.at("event").at("id").get<ObjectId>();
	CHECK(reject(client, "chord.set", {{"events", {{{"event", xId}, {"chord", "bbmaj9"}}}}}).find("bbmaj9") !=
		std::string::npos);
	run(client, "chord.set", {{"events", {{{"event", xId}, {"chord", "Bbmaj9/D"}}}}});
	run(client, "chord.add", {{"position", 2 * g_bar}, {"chord", "Gm7"}});
	CHECK(chordNames(client) == std::vector<std::string>{"C", "A#maj7(9)/D", "Gm7"});
	run(client, "chord.set", {{"events", {{{"event", xId}, {"position", 3 * g_bar}}}}});
	CHECK(chordNames(client) == std::vector<std::string>{"C", "Gm7", "A#maj7(9)/D"});
	// Adaptive voicings: Gm7 after C takes the inversion nearest to it.
	CHECK(run(client, "chord.get").at("events")[1].at("pitches") == Json{55, 58, 62, 65, 67});
	run(client, "chord.setTrack", {{"adaptiveVoicings", false}, {"octaveOffset", -1}});
	CHECK(run(client, "chord.get").at("events")[1].at("pitches") == Json{43, 55, 58, 62, 65});
	CHECK(reject(client, "chord.setTrack", {{"octaveOffset", 4}}).find("octaveOffset") != std::string::npos);
	// Scale events: chord events take their scales from them without Automatic Scales.
	const Json scale = run(client, "chord.add", {{"position", g_bar}, {"scale", "D dorian"}});
	CHECK(scale.at("event").at("scale") == "D dorian");
	CHECK(run(client, "chord.get").at("events")[1].at("scale") == "G dorian");
	run(client, "chord.setTrack", {{"automaticScales", false}});
	CHECK(run(client, "chord.get").at("events")[0].at("scale") == "C major"); // before the first scale event
	CHECK(run(client, "chord.get").at("events")[1].at("scale") == "D dorian");
	CHECK(
		reject(client, "chord.add", {{"position", g_bar}, {"scale", "E minor"}}).find("position") != std::string::npos);
	CHECK(reject(client, "chord.add", {{"position", 0}, {"scale", "C bebop"}}).find("C bebop") != std::string::npos);
	CHECK(reject(client, "chord.add", {{"chord", "C"}, {"scale", "C major"}}).find("scale") != std::string::npos);
	CHECK(run(client, "chord.get").at("scales").size() == 1);
	// Each change is an undo step; removing events and the track removes them.
	run(client, "history.undo");
	CHECK(run(client, "chord.get").at("automaticScales") == true);
	run(client, "chord.remove", {{"events", {c.at("event").at("id"), scale.at("event").at("id")}}});
	CHECK(chordNames(client) == std::vector<std::string>{"Gm7", "A#maj7(9)/D"});
	CHECK(run(client, "chord.get").at("scales").empty());
	run(client, "track.remove", {{"track", chord.at("track")}});
	CHECK(run(client, "chord.get").at("events").empty());
	run(client, "history.undo");
	CHECK(chordNames(client) == std::vector<std::string>{"Gm7", "A#maj7(9)/D"});
}

TEST_CASE("Chords to MIDI makes a part of the voicings that follows the chord track", "[integration]")
{
	Song song;
	Client& client = song.client;
	song.chords({"C", "Am", "F", "G"});
	CHECK(reject(client, "chord.toMidi", {{"track", run(client, "chord.get").at("track")}}).find("MIDI") !=
		std::string::npos);
	const Json made = run(client, "chord.toMidi", {{"track", song.track}});
	const Json& part = made.at("part");
	CHECK(part.at("position") == 0);
	CHECK(part.at("length") == 4 * g_bar); // the last chord lasts a bar
	CHECK(part.at("notes").size() == 16);
	// Adaptive voicings, the bass note an octave below.
	const Bars voicings = {{48, 60, 64, 67}, {57, 60, 64, 69}, {53, 60, 65, 69}, {55, 59, 62, 67}};
	CHECK(song.pitches() == voicings);
	// The track follows the chord track with Voicings: changing a chord changes what the part plays.
	const Json follow = run(client, "chord.get").at("follow");
	REQUIRE(follow.size() == 1);
	CHECK(follow[0].at("track") == song.track);
	CHECK(follow[0].at("mode") == "voicings");
	const Json events = run(client, "chord.get").at("events");
	run(client, "chord.set", {{"events", {{{"event", events[1].at("id")}, {"chord", "Dm"}}}}});
	CHECK(song.pitches() == Bars{{48, 60, 64, 67}, {50, 62, 65, 69}, {53, 60, 65, 69}, {55, 59, 62, 67}});
	run(client, "history.undo");
	CHECK(song.pitches() == voicings);
	// Chord events dragged onto a track: only those, and the track doesn't follow.
	const Json other = run(client, "track.add", {{"type", "instrument"}});
	const auto otherTrack = other.at("track").at("id").get<ObjectId>();
	const Json dragged =
		run(client, "chord.toMidi", {{"track", otherTrack}, {"events", {events[1].at("id"), events[2].at("id")}}});
	CHECK(dragged.at("part").at("position") == g_bar);
	CHECK(dragged.at("part").at("length") == 2 * g_bar);
	CHECK(dragged.at("part").at("notes").size() == 8);
	CHECK(run(client, "chord.get").at("follow").size() == 1);
	CHECK(reject(client, "chord.toMidi", {{"track", otherTrack}, {"events", {12345}}}).find("12345") !=
		std::string::npos);
}

TEST_CASE("MIDI tracks follow the chord track over reference progressions", "[integration]")
{
	Song song;
	Client& client = song.client;
	CHECK(reject(client, "chord.follow", {{"track", song.track}, {"mode", "chords"}}).find("chord track") !=
		std::string::npos);
	SECTION("Apply a Known Chord: a C major arpeggio over I-vi-IV-V")
	{
		song.play(Bars(4, {60, 64, 67, 72}), true);
		song.chords({"C", "Am", "F", "G"});
		run(client, "chord.follow", {{"track", song.track}, {"mode", "chords"}, {"sync", "known"}, {"chord", "C"}});
		CHECK(song.pitches() == Bars{{60, 64, 67, 72}, {57, 60, 64, 69}, {65, 69, 72, 77}, {55, 59, 62, 67}});
		run(client, "chord.follow", {{"track", song.track}, {"mode", "rootNotes"}});
		CHECK(song.pitches() == Bars{{60, 64, 67, 72}, {57, 61, 64, 69}, {65, 69, 72, 77}, {55, 59, 62, 67}});
		run(client, "chord.follow", {{"track", song.track}, {"mode", "off"}});
		CHECK(song.pitches() == Bars(4, {60, 64, 67, 72}));
		run(client, "history.undo");
		CHECK(song.pitches()[1] == std::vector<int>{57, 61, 64, 69});
	}
	SECTION("Follow Directly: I-vi-IV-V in C moves to D with the chord events")
	{
		song.play(triads());
		song.chords({"C", "Am", "F", "G"});
		run(client, "chord.follow", {{"track", song.track}, {"mode", "chords"}});
		CHECK(song.pitches() == triads()); // the notes already match the chord track
		const Json events = run(client, "chord.get").at("events");
		const std::vector<std::string> inD = {"D", "Bm", "G", "A"};
		Json changes = Json::array();
		for (size_t i = 0; i < inD.size(); ++i)
			changes.push_back({{"event", events[i].at("id")}, {"chord", inD[i]}});
		run(client, "chord.set", {{"events", changes}});
		CHECK(song.pitches() == Bars{{62, 66, 69}, {59, 62, 66}, {67, 71, 74}, {57, 61, 64}});
		// Scales: the vi chord's A minor over B minor.
		run(client, "chord.follow", {{"track", song.track}, {"mode", "scales"}});
		CHECK(song.pitches()[1] == std::vector<int>{59, 62, 66});
	}
	SECTION("Analyze Chords: the part's own chords follow I-vi-IV-V in D")
	{
		song.play(triads());
		song.chords({"D", "Bm", "G", "A"});
		run(client, "chord.follow", {{"track", song.track}, {"mode", "chords"}, {"sync", "analyze"}});
		CHECK(song.pitches() == Bars{{62, 66, 69}, {59, 62, 66}, {67, 71, 74}, {57, 61, 64}});
		// Chords & Scales over a minor progression: thirds become minor thirds.
		const Json events = run(client, "chord.get").at("events");
		run(client, "chord.set", {{"events", {{{"event", events[0].at("id")}, {"chord", "Dm"}}}}});
		run(client, "chord.follow", {{"track", song.track}, {"mode", "chordsAndScales"}});
		CHECK(song.pitches()[0] == std::vector<int>{62, 65, 69});
	}
	SECTION("the commands refuse what they can't follow")
	{
		song.chords({"C"});
		CHECK(reject(client, "chord.follow", {{"track", song.track}, {"mode", "sideways"}}).find("mode") !=
			std::string::npos);
		CHECK(reject(client, "chord.follow", {{"track", song.track}, {"mode", "chords"}, {"sync", "known"}})
				  .find("chord") != std::string::npos);
		const auto audio = run(client, "track.add", {{"type", "audio"}}).at("track").at("id").get<ObjectId>();
		CHECK(reject(client, "chord.follow", {{"track", audio}, {"mode", "chords"}}).find("MIDI") != std::string::npos);
	}
}

TEST_CASE("Create Chord Events makes chord events from MIDI notes", "[integration]")
{
	Song song;
	Client& client = song.client;
	// C, Am7, F/A, G7 with a short passing note in the second bar; the chord track comes with the events.
	song.play({{60, 64, 67}, {57, 60, 64, 67}, {57, 60, 65}, {55, 59, 62, 65}});
	run(client, "note.add",
		{{"part", song.part},
			{"notes",
				{{{"position", g_bar + (3 * g_beatTicks)}, {"length", g_beatTicks / 8}, {"pitch", 71},
					{"velocity", 0.5}}}}});
	const Json created = run(client, "chord.fromMidi", {{"parts", {song.part}}, {"minLength", g_beatTicks / 4}});
	CHECK(created.at("events").size() == 4);
	CHECK(chordNames(client) == std::vector<std::string>{"C", "Am7", "F/A", "G7"});
	std::vector<int64_t> positions;
	const Json chord = run(client, "chord.get");
	for (const Json& event : chord.at("events"))
		positions.push_back(event.at("position").get<int64_t>());
	CHECK(positions == std::vector<int64_t>{0, g_bar, 2 * g_bar, 3 * g_bar});
	// Again without bass notes and tensions, from the track: the events in its range are replaced.
	run(client, "chord.fromMidi",
		{{"tracks", {song.track}}, {"includeBass", false}, {"includeTensions", false}, {"minLength", g_beatTicks / 4}});
	CHECK(chordNames(client) == std::vector<std::string>{"C", "Am", "F", "G"});
	// Short notes count unless left out: Am7 with B makes Am7(9) on the fourth beat.
	run(client, "chord.fromMidi", {{"parts", {song.part}}});
	CHECK(chordNames(client) == std::vector<std::string>{"C", "Am7", "Am7(9)", "Am7", "F/A", "G7"});
	CHECK(reject(client, "chord.fromMidi", Json::object()).find("parts") != std::string::npos);
}

TEST_CASE("the chord track is saved with the project", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string path = (dir.path() / "Song.dawproject").u8string();
	Json saved;
	Bars played;
	{
		Song song;
		song.play(triads());
		// A linear chord track: its positions are samples.
		run(song.client, "track.add", {{"type", "chord"}, {"timeBase", "linear"}});
		const std::vector<std::string> names = {"C", "Am", "F", "G"};
		for (size_t bar = 0; bar < names.size(); ++bar)
			run(song.client, "chord.add",
				{{"position", static_cast<int64_t>(bar) * g_barSamples}, {"chord", names[bar]}});
		run(song.client, "chord.add", {{"position", g_barSamples}, {"scale", "A harmonicMinor"}});
		run(song.client, "chord.setTrack",
			{{"octaveOffset", 1}, {"adaptiveVoicings", false}, {"automaticScales", false}});
		run(song.client, "chord.follow",
			{{"track", song.track}, {"mode", "rootNotes"}, {"sync", "known"}, {"chord", "D"}});
		saved = run(song.client, "chord.get");
		played = song.pitches();
		run(song.client, "project.save", {{"path", path}});
	}
	// From D to C, A, F and G by their roots, the nearer way.
	CHECK(played == Bars{{58, 62, 65}, {52, 55, 59}, {68, 72, 75}, {60, 64, 67}});
	Song song;
	run(song.client, "project.load", {{"path", path}});
	CHECK(run(song.client, "chord.get") == saved);
	song.channel = run(song.client, "project.get").at("tracks")[0].at("channel").get<ObjectId>();
	CHECK(song.pitches() == played);
}

TEST_CASE("a project file's chord track is read as the chord commands would make it", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string path = (dir.path() / "Song.dawproject").u8string();
	{
		Song song;
		run(song.client, "track.add", {{"type", "chord"}});
		song.chords({"C", "Am"});
		run(song.client, "chord.add", {{"position", g_bar}, {"scale", "A minor"}});
		run(song.client, "chord.follow", {{"track", song.track}, {"mode", "chords"}});
		run(song.client, "project.save", {{"path", path}});
	}
	// Text no command takes, markup among it, a second event where one is, events before the project start and
	// settings no command takes (#939).
	std::map<std::string, std::string> entries = readZip(path);
	Json extension = Json::parse(entries.at("gnubase.json"));
	Json& chords = extension.at("project").at("chords");
	chords.at("events")[0]["chord"] = "<div class=\"x\">C</div>";
	Json second = chords.at("events")[1];
	second["id"] = second.at("id").get<ObjectId>() + 1000;
	second["chord"] = "F";
	chords.at("events").push_back(second);
	chords.at("scales")[0]["scale"] = "<b>A minor</b>";
	Json early = chords.at("events")[1];
	early["id"] = early.at("id").get<ObjectId>() + 2000;
	early["position"] = -g_beatTicks;
	chords.at("events").push_back(early);
	Json earlyScale = early;
	earlyScale.erase("chord");
	earlyScale["scale"] = "C major";
	earlyScale["id"] = early.at("id").get<ObjectId>() + 1;
	chords.at("scales").push_back(earlyScale);
	chords.at("follow")[0]["mode"] = "sideways";
	chords.at("follow")[0]["voicings"] = "harp";
	chords.at("follow")[0]["liveInput"] = "everything";
	chords["voicing"] = {{"library", "banjo"}, {"lowest", 300}};
	entries["gnubase.json"] = extension.dump();
	writeZip(path, entries);

	Song song;
	run(song.client, "project.load", {{"path", path}});
	CHECK(chordNames(song.client) == std::vector<std::string>{"X", "Am"});
	const Json chord = run(song.client, "chord.get");
	CHECK(chord.at("scales").empty());
	REQUIRE(chord.at("follow").size() == 1);
	CHECK(chord.at("follow")[0].at("mode") == "off"); // not chords, as an unknown mode played
	CHECK(chord.at("follow")[0].at("voicings") == "chordTrack");
	CHECK(chord.at("follow")[0].at("liveInput") == "off");
	CHECK(chord.at("voicing").at("library") == "basic");
	CHECK(chord.at("voicing").at("lowest") == 52);
	// The commands still take the events.
	run(song.client, "chord.add", {{"position", 2 * g_bar}, {"chord", "F"}});
	run(song.client, "chord.set", {{"events", {{{"event", chord.at("events")[0].at("id")}, {"chord", "G"}}}}});
	CHECK(chordNames(song.client) == std::vector<std::string>{"G", "Am", "F"});
}

TEST_CASE("Mute Chord Track: nothing follows it while it is muted", "[integration]")
{
	Song song;
	Client& client = song.client;
	song.play(triads());
	song.chords({"D", "Bm", "G", "A"});
	run(client, "chord.follow", {{"track", song.track}, {"mode", "chords"}, {"sync", "analyze"}});
	const Bars inD = {{62, 66, 69}, {59, 62, 66}, {67, 71, 74}, {57, 61, 64}};
	CHECK(song.pitches() == inD);
	const Json muted = run(client, "chord.setTrack", {{"mute", true}});
	CHECK(muted.at("mute") == true);
	CHECK(song.pitches() == triads());
	run(client, "history.undo");
	CHECK(song.pitches() == inD);
}

TEST_CASE("a track's Follow Chord Track settings go with it and come with its copy", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string path = (dir.path() / "Song.dawproject").u8string();
	Song song;
	Client& client = song.client;
	song.chords({"C"});
	run(client, "chord.follow", {{"track", song.track}, {"mode", "scales"}});
	// A copy follows as its original does.
	const auto copy = run(client, "track.duplicate", {{"track", song.track}}).at("track").at("id").get<ObjectId>();
	Json follow = run(client, "chord.get").at("follow");
	REQUIRE(follow.size() == 2);
	CHECK(follow[1].at("track") == copy);
	CHECK(follow[1].at("mode") == "scales");
	CHECK(follow[1].at("source") == follow[0].at("source"));
	// Removing a track removes them, also from the project file; undo brings them back.
	run(client, "track.remove", {{"track", song.track}});
	follow = run(client, "chord.get").at("follow");
	REQUIRE(follow.size() == 1);
	CHECK(follow[0].at("track") == copy);
	run(client, "project.save", {{"path", path}});
	const Json saved = Json::parse(readZip(path).at("gnubase.json")).at("project").at("chords").at("follow");
	REQUIRE(saved.size() == 1);
	CHECK(saved[0].at("track") == copy);
	run(client, "history.undo");
	CHECK(run(client, "chord.get").at("follow").size() == 2);
}

TEST_CASE("drum-mapped tracks don't follow the chord track", "[integration]")
{
	Song song;
	Client& client = song.client;
	song.play(Bars(4, {36, 38, 42}));
	song.chords({"C", "D", "E", "F"});
	run(client, "chord.follow", {{"track", song.track}, {"mode", "chords"}, {"sync", "known"}, {"chord", "C"}});
	CHECK(song.pitches() != Bars(4, {36, 38, 42}));
	const Json maps = run(client, "project.get").at("drumMaps");
	REQUIRE(!maps.empty());
	run(client, "track.set", {{"track", song.track}, {"drumMap", maps[0].at("id")}});
	CHECK(song.pitches() == Bars(4, {36, 38, 42})); // the GM map plays each note as itself
}

TEST_CASE("Chords to MIDI: the last chord lasts a bar, also off the bar line", "[integration]")
{
	Song song;
	Client& client = song.client;
	run(client, "chord.add", {{"position", 0}, {"chord", "C"}});
	run(client, "chord.add", {{"position", g_bar + (2 * g_beatTicks)}, {"chord", "G"}});
	const Json part = run(client, "chord.toMidi", {{"track", song.track}}).at("part");
	CHECK(part.at("length") == (2 * g_bar) + (2 * g_beatTicks));
	for (const Json& note : part.at("notes"))
		if (note.at("position") != 0)
			CHECK(note.at("length") == g_bar);
}

TEST_CASE("Shift Chords: chord events take notes played early", "[integration]")
{
	Song song;
	Client& client = song.client;
	// C in the first bar, then a C a sixteenth before the second bar, over C and D.
	song.chords({"C", "D"});
	run(client, "note.add",
		{{"part", song.part},
			{"notes",
				{{{"position", g_bar - (g_beatTicks / 4)}, {"length", g_beatTicks}, {"pitch", 60},
					{"velocity", 0.75}}}}});
	run(client, "chord.follow", {{"track", song.track}, {"mode", "rootNotes"}, {"sync", "known"}, {"chord", "C"}});
	CHECK(song.pitches()[0] == std::vector<int>{60});
	run(client, "chord.setTrack", {{"shiftChords", -g_beatTicks / 2}});
	CHECK(song.pitches()[0] == std::vector<int>{62});
	CHECK(run(client, "chord.get").at("shiftChords") == -g_beatTicks / 2);
	CHECK(
		reject(client, "chord.setTrack", {{"shiftChords", 2 * g_beatTicks}}).find("shiftChords") != std::string::npos);
}

TEST_CASE("voicing libraries, This Track's Voicings and Single Voice with a master", "[integration]")
{
	Song song;
	Client& client = song.client;
	song.chords({"C", "Am", "F", "G"});
	run(client, "chord.setTrack",
		{{"adaptiveVoicings", false}, {"voicing", {{"library", "piano"}, {"style", "triads"}}}});
	Json chord = run(client, "chord.get");
	CHECK(chord.at("voicing").at("library") == "piano");
	CHECK(chord.at("events")[0].at("pitches") == Json{36, 60, 64, 67});
	CHECK(reject(client, "chord.setTrack", {{"voicing", {{"style", "polka"}}}}).find("polka") != std::string::npos);
	CHECK(reject(client, "chord.setTrack", {{"voicing", {{"lowest", 80}, {"highest", 84}}}}).find("octave") !=
		std::string::npos);
	run(client, "chord.setTrack", {{"voicing", {{"library", "guitar"}}}});
	CHECK(run(client, "chord.get").at("events")[0].at("pitches") == Json{48, 52, 55, 60, 64}); // the open C
	run(client, "chord.setTrack", {{"voicing", {{"library", "basic"}}}});

	// Single Voice: the soprano of each chord's voicing; with the guitar's voicings as This Track's Voicings.
	song.play(Bars(4, {60}));
	run(client, "chord.follow", {{"track", song.track}, {"mode", "singleVoice"}, {"voice", "soprano"}});
	CHECK(song.pitches() == Bars{{67}, {76}, {72}, {74}}); // the basic voicings, not adaptive
	run(client, "chord.follow", {{"track", song.track}, {"voice", "bass"}});
	CHECK(song.pitches() == Bars{{48}, {57}, {53}, {55}});
	run(client, "chord.follow", {{"track", song.track}, {"voicings", "guitar"}});
	CHECK(song.pitches()[0] == std::vector<int>{48});
	// A track with a master takes the master's voicings.
	const Json other = run(client, "track.add", {{"type", "midi"}});
	const auto otherTrack = other.at("track").at("id").get<ObjectId>();
	run(client, "chord.follow", {{"track", song.track}, {"voicings", "chordTrack"}, {"master", otherTrack}});
	run(client, "chord.follow", {{"track", otherTrack}, {"mode", "off"}, {"voicings", "guitar"}});
	CHECK(song.pitches()[0] == std::vector<int>{48});
	run(client, "chord.follow", {{"track", otherTrack}, {"voicings", "piano"}});
	CHECK(song.pitches()[0] == std::vector<int>{36});
	CHECK(reject(client, "chord.follow", {{"track", song.track}, {"master", song.track}}).find("master") !=
		std::string::npos);
	chord = run(client, "chord.get");
	CHECK(chord.at("follow")[0].at("master") == otherTrack);
	CHECK(chord.at("follow")[0].at("voice") == "bass");
	// The master's settings go with it.
	run(client, "track.remove", {{"track", otherTrack}});
	CHECK(run(client, "chord.get").at("follow")[0].at("master") == 0);
}

TEST_CASE("Create Chord Events with Detect Arpeggios and Interpret Sustain Pedal", "[integration]")
{
	Song song;
	Client& client = song.client;
	SECTION("arpeggios: C, Am, F and G a note a beat")
	{
		song.play({{60, 64, 67, 72}, {57, 60, 64, 69}, {53, 57, 60, 65}, {55, 59, 62, 67}}, true);
		run(client, "chord.fromMidi", {{"parts", {song.part}}});
		CHECK(chordNames(client).empty()); // one note at a time makes no chord
		run(client, "chord.fromMidi", {{"parts", {song.part}}, {"detectArpeggios", true}});
		CHECK(chordNames(client) == std::vector<std::string>{"C", "Am", "F", "G"});
		std::vector<int64_t> positions;
		const Json chord = run(client, "chord.get");
		for (const Json& event : chord.at("events"))
			positions.push_back(event.at("position").get<int64_t>());
		CHECK(positions == std::vector<int64_t>{0, g_bar, 2 * g_bar, 3 * g_bar});
	}
	SECTION("the sustain pedal holds a beat-long C, E and G")
	{
		run(client, "note.add",
			{{"part", song.part},
				{"notes",
					{{{"position", 0}, {"length", g_beatTicks / 2}, {"pitch", 60}, {"velocity", 0.75}},
						{{"position", g_beatTicks}, {"length", g_beatTicks / 2}, {"pitch", 64}, {"velocity", 0.75}},
						{{"position", 2 * g_beatTicks}, {"length", g_beatTicks / 2}, {"pitch", 67},
							{"velocity", 0.75}}}}});
		run(client, "controller.add",
			{{"part", song.part},
				{"controllers",
					{{{"position", 0}, {"number", 64}, {"value", 1.0}},
						{{"position", g_bar}, {"number", 64}, {"value", 0.0}}}}});
		run(client, "chord.fromMidi", {{"parts", {song.part}}});
		CHECK(chordNames(client).empty());
		run(client, "chord.fromMidi", {{"parts", {song.part}}, {"sustainPedal", true}});
		CHECK(chordNames(client) == std::vector<std::string>{"C"});
		CHECK(run(client, "chord.get").at("events")[0].at("position") == 2 * g_beatTicks);
	}
}

TEST_CASE("Map to Chord Track and Assign Voices to Notes change the notes", "[integration]")
{
	Song song;
	Client& client = song.client;
	song.play(triads());
	song.chords({"D", "Bm", "G", "A"});
	CHECK(reject(client, "chord.map", {{"parts", {12345}}}).find("12345") != std::string::npos);
	const Json mapped = run(client, "chord.map", {{"parts", {song.part}}, {"mode", "chords"}});
	CHECK(mapped.at("notes").size() == 12);
	const Bars inD = {{62, 66, 69}, {59, 62, 66}, {67, 71, 74}, {57, 61, 64}};
	CHECK(song.pitches() == inD);
	run(client, "history.undo");
	CHECK(song.pitches() == triads());
	// Assign Voices: each note to the nearest pitch of the chord's voicing.
	run(client, "chord.setTrack", {{"adaptiveVoicings", false}});
	run(client, "chord.assignVoices", {{"parts", {song.part}}});
	const Bars voiced = song.pitches();
	const Json events = run(client, "chord.get").at("events");
	for (size_t bar = 0; bar < voiced.size(); ++bar)
		for (const int pitch : voiced[bar])
		{
			const Json& voicing = events[bar].at("pitches");
			CHECK(std::find(voicing.begin(), voicing.end(), pitch) != voicing.end());
		}
}

TEST_CASE("chord events from audio events", "[integration]")
{
	const gnubase::test::TempDir dir;
	Song song;
	Client& client = song.client;
	// Two bars at 120 BPM: C major (C4 E4 G4), then A minor (A3 C4 E4), sines, then a bar of silence.
	gnubase::test::Audio audio;
	const int rate = 48000;
	const auto frames = static_cast<size_t>(3 * g_barSamples);
	audio.samples.resize(frames);
	const std::vector<std::vector<double>> bars = {{261.63, 329.63, 392.0}, {220.0, 261.63, 329.63}};
	for (size_t i = 0; i < frames; ++i)
	{
		const size_t bar = i / static_cast<size_t>(g_barSamples);
		if (bar >= bars.size())
			continue;
		for (const double frequency : bars[bar])
			audio.samples[i] +=
				static_cast<float>(0.2 * std::sin(2.0 * 3.14159265358979 * frequency * static_cast<double>(i) / rate));
	}
	const auto file = dir.path() / "chords.wav";
	gnubase::test::writeWav(file, audio);
	const auto audioTrack = run(client, "track.add", {{"type", "audio"}}).at("track").at("id").get<ObjectId>();
	const Json media = run(client, "media.add", {{"path", file.u8string()}}).at("media");
	const Json clip = run(client, "clip.add", {{"media", media.at("id")}}).at("clip");
	const Json event = run(client, "audioEvent.add",
		{{"track", audioTrack}, {"clip", clip.at("id")}, {"position", 0},
			{"length",
				frames}}).at("event");
	run(client, "chord.add", {{"position", g_bar}, {"chord", "F"}});     // replaced
	run(client, "chord.add", {{"position", 4 * g_bar}, {"chord", "G"}}); // after the event, kept
	const Json made = run(client, "chord.fromAudio", {{"events", {event.at("id")}}});
	CHECK(made.at("events").size() == 2);
	CHECK(chordNames(client) == std::vector<std::string>{"C", "Am", "G"});
	const Json events = run(client, "chord.get").at("events");
	CHECK(events[0].at("position") == 0);
	CHECK(events[1].at("position") == g_bar);
	CHECK(events[0].at("audioEvent") == event.at("id"));
	REQUIRE(!events[0].at("detected").empty());
	CHECK(events[0].at("detected")[0] == "C");
	CHECK(events[1].at("detected")[0] == "Am");
	CHECK(events[2].at("audioEvent") == 0);
	CHECK(reject(client, "chord.fromAudio", {{"events", {12345}}}).find("12345") != std::string::npos);
}

TEST_CASE("recording chord events from the notes a MIDI track records", "[integration]")
{
	Song song;
	Client& client = song.client;
	run(client, "track.add", {{"type", "chord"}});
	run(client, "chord.add", {{"position", 3 * g_bar}, {"chord", "E"}}); // outside the take, kept
	const auto note = [](int64_t bar, int pitch)
	{ return Json{{"position", bar * g_barSamples}, {"length", g_barSamples}, {"pitch", pitch}, {"velocity", 0.8}}; };
	const Json recorded = {{"tracks",
		{{{"track", song.track}, {"takes", {{{"position", 0}, {"length", 2 * g_barSamples}}}},
			{"notes", {note(0, 60), note(0, 64), note(0, 67), note(1, 62), note(1, 65), note(1, 69)}}}}}};
	run(client, "recording.add", recorded);
	CHECK(chordNames(client) == std::vector<std::string>{"E"}); // the chord track wasn't record-enabled
	run(client, "history.undo");
	run(client, "chord.setTrack", {{"recordEnable", true}});
	run(client, "recording.add", recorded);
	CHECK(chordNames(client) == std::vector<std::string>{"C", "Dm", "E"});
}

namespace
{
	std::vector<std::vector<int>> notesPlayed(const Json& capture)
	{
		std::vector<std::vector<int>> notes;
		for (const Json& captured : capture.at("captured"))
		{
			const Json& bytes = captured.at("bytes");
			const int status = bytes.at(0).get<int>() & 0xF0;
			if (status == 0x90 || status == 0x80)
				notes.push_back({status, bytes.at(1).get<int>()});
		}
		return notes;
	}

	// An instrument track playing the note probe that monitors, on the dummy device, and a chord track with C.
	struct Rig
	{
		Rig() : engine(gnubase::audio::makeBackends(), {}, std::vector<std::unique_ptr<gnubase::midi::Backend>>())
		{
			run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}});
			const Json added = run(client, "track.add", {{"type", "instrument"}});
			track = added.at("track").at("id").get<ObjectId>();
			channel = added.at("channel").at("id").get<ObjectId>();
			run(client, "channel.set", {{"channel", channel}, {"instrument", "noteProbe"}, {"monitor", true}});
			run(client, "chord.add", {{"position", 0}, {"chord", "C"}});
			run(client, "audio.render", {{"frames", 64}}); // the new graph
			run(client, "midi.capture", {{"start", true}});
		}

		std::vector<std::vector<int>> play(int pitch)
		{
			run(client, "midi.inject",
				{{"channel", channel},
					{"messages",
						{{{"position", 0}, {"bytes", {0x90, pitch, 100}}},
							{{"position", 0}, {"bytes", {0x80, pitch, 0}}}}}});
			run(client, "audio.render", {{"frames", 64}});
			return notesPlayed(run(client, "midi.capture"));
		}

		Engine engine;
		Client client{engine};
		ObjectId track = 0;
		ObjectId channel = 0;
	};
}

TEST_CASE("Live Input moves the notes played into a track to the chord track's chords or scales", "[integration]")
{
	Rig rig;
	Client& client = rig.client;
	CHECK(rig.play(63) == std::vector<std::vector<int>>{{0x90, 63}, {0x80, 63}});
	run(client, "chord.follow", {{"track", rig.track}, {"liveInput", "chords"}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(run(client, "chord.get").at("follow")[0].at("mode") == "off");
	CHECK(rig.play(63) == std::vector<std::vector<int>>{{0x90, 64}, {0x80, 64}}); // D# to E
	run(client, "chord.follow", {{"track", rig.track}, {"liveInput", "scales"}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(rig.play(63) == std::vector<std::vector<int>>{{0x90, 62}, {0x80, 62}}); // C major: D, down on a tie
	CHECK(reject(client, "chord.follow", {{"track", rig.track}, {"liveInput", "loud"}}).find("liveInput") !=
		std::string::npos);
	// Not while the chord track is muted.
	run(client, "chord.setTrack", {{"mute", true}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(rig.play(63) == std::vector<std::vector<int>>{{0x90, 63}, {0x80, 63}});
}

TEST_CASE("auditioning chord events on the audition track, and Acoustic Feedback", "[integration]")
{
	Rig rig;
	Client& client = rig.client;
	const Json events = run(client, "chord.get").at("events");
	CHECK(reject(client, "chord.audition", {{"event", events[0].at("id")}}).find("auditioning") != std::string::npos);
	CHECK(reject(client, "chord.setTrack", {{"audition", 12345}}).find("MIDI") != std::string::npos);
	run(client, "chord.setTrack", {{"audition", rig.track}});
	const Json played = run(client, "chord.audition", {{"event", events[0].at("id")}});
	CHECK(played.at("pitches") == Json{48, 60, 64, 67});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(notesPlayed(run(client, "midi.capture")) ==
		std::vector<std::vector<int>>{{0x90, 48}, {0x90, 60}, {0x90, 64}, {0x90, 67}});
	run(client, "chord.audition", {{"on", false}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(notesPlayed(run(client, "midi.capture")) ==
		std::vector<std::vector<int>>{{0x80, 48}, {0x80, 60}, {0x80, 64}, {0x80, 67}});
	// A scale event plays its notes.
	const Json scale = run(client, "chord.add", {{"position", 0}, {"scale", "D dorian"}}).at("event");
	CHECK(run(client, "chord.audition", {{"event", scale.at("id")}}).at("pitches") ==
		Json{62, 64, 65, 67, 69, 71, 72, 74});
	run(client, "chord.audition", {{"on", false}});
	// Acoustic Feedback: the chord events play on the audition track, a bar for the last.
	run(client, "chord.setTrack", {{"acousticFeedback", true}});
	std::vector<int> pitches;
	for (const RenderChannel& render : rig.engine.renderSnapshots().acquire().channels)
		if (render.id == rig.channel)
			for (const RenderNote& note : render.notes)
			{
				pitches.push_back(note.pitch);
				CHECK(note.start == 0);
				CHECK(note.end == g_barSamples);
			}
	CHECK(pitches == std::vector<int>{48, 60, 64, 67});
	run(client, "chord.setTrack", {{"mute", true}});
	CHECK(reject(client, "chord.audition", {{"chord", "C"}}).find("muted") != std::string::npos);
}

TEST_CASE("a chord held on an audition track that is removed doesn't block auditioning on another", "[integration]")
{
	Rig rig;
	Client& client = rig.client;
	run(client, "chord.setTrack", {{"audition", rig.track}});
	run(client, "chord.audition", {{"chord", "C"}});
	run(client, "track.remove", {{"track", rig.track}});
	const Json added = run(client, "track.add", {{"type", "instrument"}});
	const auto channel = added.at("channel").at("id").get<ObjectId>();
	run(client, "channel.set", {{"channel", channel}, {"instrument", "noteProbe"}, {"monitor", true}});
	run(client, "chord.setTrack", {{"audition", added.at("track").at("id")}});
	run(client, "audio.render", {{"frames", 64}}); // the new graph
	run(client, "midi.capture");                   // drops what was captured so far
	CHECK(run(client, "chord.audition", {{"chord", "C"}}).at("pitches") == Json{48, 60, 64, 67});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(notesPlayed(run(client, "midi.capture")) ==
		std::vector<std::vector<int>>{{0x90, 48}, {0x90, 60}, {0x90, 64}, {0x90, 67}});
	run(client, "chord.audition", {{"on", false}});
	run(client, "audio.render", {{"frames", 64}});
	CHECK(notesPlayed(run(client, "midi.capture")) ==
		std::vector<std::vector<int>>{{0x80, 48}, {0x80, 60}, {0x80, 64}, {0x80, 67}});
}
