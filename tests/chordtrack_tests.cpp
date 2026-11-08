// SPDX-License-Identifier: GPL-3.0-or-later
// The chord track (#173, reference manual pp. 165, 1319-1345): chord and scale events, voicings, converting chord
// events to MIDI, MIDI tracks that follow the chord track over reference progressions, chord events from MIDI and the
// project file.
#include "engine/document/document.h"
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;

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
		run(song.client, "track.add", {{"type", "chord"}, {"timeBase", "linear"}});
		song.chords({"C", "Am", "F", "G"});
		run(song.client, "chord.add", {{"position", g_bar}, {"scale", "A harmonicMinor"}});
		run(song.client, "chord.setTrack",
			{{"octaveOffset", 1}, {"adaptiveVoicings", false}, {"automaticScales", false}});
		run(song.client, "chord.follow",
			{{"track", song.track}, {"mode", "rootNotes"}, {"sync", "known"}, {"chord", "D"}});
		saved = run(song.client, "chord.get");
		played = song.pitches();
		run(song.client, "project.save", {{"path", path}});
	}
	CHECK(played[0] == std::vector<int>{58, 62, 65}); // D to C: a whole tone down
	Song song;
	run(song.client, "project.load", {{"path", path}});
	CHECK(run(song.client, "chord.get") == saved);
	song.channel = run(song.client, "project.get").at("tracks")[0].at("channel").get<ObjectId>();
	CHECK(song.pitches() == played);
}
