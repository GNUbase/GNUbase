// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/midi/midivalues.h"
#include "engine/midi/modifiers.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;

namespace
{
	// 120 BPM at 48 kHz: a beat is 24000 samples.
	constexpr int64_t g_beat = 24000;
	constexpr int64_t g_beatTicks = 1720320;

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters)
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	// A note as modifyNote plays it: its pitch and MIDI 1.0 velocity, or -1 and -1 if it doesn't play.
	std::pair<int, int> played(const midi::Modifiers& modifiers, int pitch, int velocity, uint64_t seed = 1)
	{
		auto key = static_cast<uint8_t>(pitch);
		uint16_t value = midi::velocityFromMidi1(static_cast<uint8_t>(velocity));
		if (!midi::modifyNote(modifiers, seed, key, value))
			return {-1, -1};
		return {key, midi::velocityToMidi1(value)};
	}

	// An instrument track playing the note probe, with a part of four beats from 0 holding notes on every beat.
	struct ModifiedTrack
	{
		ModifiedTrack() : engine(gnubase::audio::makeBackends())
		{
			run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
			const Json added = run(client, "track.add", {{"type", "instrument"}});
			channel = added.at("channel").at("id").get<ObjectId>();
			run(client, "channel.set", {{"channel", channel}, {"instrument", "noteProbe"}});
			const ObjectId track = added.at("track").at("id").get<ObjectId>();
			part = run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 4 * g_beatTicks}})
					   .at("part")
					   .at("id")
					   .get<ObjectId>();
		}

		// Adds notes of half a beat, one on each beat from 0, with these pitches and MIDI 1.0 velocities.
		void add(const std::vector<std::pair<int, int>>& notes)
		{
			Json list = Json::array();
			for (size_t i = 0; i < notes.size(); ++i)
				list.push_back({{"position", static_cast<int64_t>(i) * g_beatTicks}, {"length", g_beatTicks / 2},
					{"pitch", notes[i].first}, {"velocity", notes[i].second / 127.0}});
			run(client, "note.add", {{"part", part}, {"notes", std::move(list)}});
		}

		Json modify(Json modifiers)
		{
			return run(client, "channel.set", {{"channel", channel}, {"modifiers", std::move(modifiers)}})
				.at("channel")
				.at("modifiers");
		}

		// The notes the track plays, from the render snapshot.
		std::vector<RenderNote> notes()
		{
			std::vector<RenderNote> notes;
			for (const RenderChannel& render : engine.renderSnapshots().acquire().channels)
				if (render.id == channel)
					notes = render.notes;
			return notes;
		}

		Engine engine;
		Client client{engine};
		ObjectId channel = 0;
		ObjectId part = 0;
	};

	std::vector<int> pitches(const std::vector<RenderNote>& notes)
	{
		std::vector<int> result;
		for (const RenderNote& note : notes)
			result.push_back(note.pitch);
		return result;
	}

	std::vector<int> velocities(const std::vector<RenderNote>& notes)
	{
		std::vector<int> result;
		for (const RenderNote& note : notes)
			result.push_back(midi::velocityToMidi1(note.velocity));
		return result;
	}
}

TEST_CASE("transpose moves notes by semitones; notes beyond 0 to 127 don't play", "[unit]")
{
	midi::Modifiers modifiers;
	CHECK(played(modifiers, 60, 100) == std::pair{60, 100});
	modifiers.transpose = 7;
	CHECK(played(modifiers, 60, 100) == std::pair{67, 100});
	CHECK(played(modifiers, 121, 100) == std::pair{-1, -1});
	modifiers.transpose = -12;
	CHECK(played(modifiers, 12, 100) == std::pair{0, 100});
	CHECK(played(modifiers, 11, 100) == std::pair{-1, -1});
}

TEST_CASE("velocity shift adds to velocities, which stay from 1 to 127", "[unit]")
{
	midi::Modifiers modifiers;
	modifiers.velocityShift = 10;
	CHECK(played(modifiers, 60, 100) == std::pair{60, 110});
	CHECK(played(modifiers, 60, 120) == std::pair{60, 127});
	modifiers.velocityShift = -50;
	CHECK(played(modifiers, 60, 30) == std::pair{60, 1});
}

TEST_CASE("velocity compression multiplies velocities before the shift, and the shift expands them", "[unit]")
{
	midi::Modifiers modifiers;
	modifiers.velocityCompression = {1, 2};
	CHECK(played(modifiers, 60, 100) == std::pair{60, 50});
	CHECK(played(modifiers, 60, 41) == std::pair{60, 21}); // rounded
	modifiers.velocityCompression = {3, 2};
	modifiers.velocityShift = -40;
	CHECK(played(modifiers, 60, 100) == std::pair{60, 110});
	CHECK(played(modifiers, 60, 60) == std::pair{60, 50});
	CHECK(played(modifiers, 60, 120) == std::pair{60, 127}); // the maximum stays 127
}

TEST_CASE("length compression multiplies lengths, which stay at least a tick", "[unit]")
{
	midi::Modifiers modifiers;
	CHECK(midi::modifyLength(modifiers, 1, 960) == 960);
	modifiers.lengthCompression = {3, 4};
	CHECK(midi::modifyLength(modifiers, 1, 960) == 720);
	modifiers.lengthCompression = {1, 16};
	CHECK(midi::modifyLength(modifiers, 1, 8) == 1);
	modifiers.bypass = true;
	CHECK(midi::modifyLength(modifiers, 1, 960) == 960);
}

TEST_CASE("random modifiers add a value from their minimum to their maximum, the same for the same seed", "[unit]")
{
	midi::Modifiers modifiers;
	modifiers.random[0] = {midi::RandomTarget::Pitch, -2, 2};
	modifiers.random[1] = {midi::RandomTarget::Velocity, 5, 10};
	std::set<int> keys;
	std::set<int> values;
	for (uint64_t seed = 0; seed < 200; ++seed)
	{
		const auto [key, value] = played(modifiers, 60, 100, midi::noteSeed(7, seed));
		CHECK(played(modifiers, 60, 100, midi::noteSeed(7, seed)) == std::pair{key, value});
		keys.insert(key);
		values.insert(value);
	}
	CHECK(keys == std::set<int>{58, 59, 60, 61, 62});
	CHECK(values == std::set<int>{105, 106, 107, 108, 109, 110});

	// Both generators on one target add up; positions and lengths are for the parts' notes.
	modifiers.random[1] = {midi::RandomTarget::Pitch, 12, 12};
	CHECK(played(modifiers, 60, 100).first >= 70);
	modifiers.random = {};
	modifiers.random[0] = {midi::RandomTarget::Position, -100, 100};
	modifiers.random[1] = {midi::RandomTarget::Length, 50, 50};
	CHECK(midi::randomOffset(modifiers, midi::RandomTarget::Position, 3) >= -100);
	CHECK(midi::randomOffset(modifiers, midi::RandomTarget::Position, 3) <= 100);
	CHECK(midi::modifyLength(modifiers, 3, 960) == 1010);
	CHECK(played(modifiers, 60, 100) == std::pair{60, 100});
}

TEST_CASE("ranges limit or filter velocities and pitches, in order after the other modifiers", "[unit]")
{
	midi::Modifiers modifiers;
	modifiers.range[0] = {midi::RangeTarget::VelocityLimit, 40, 90};
	CHECK(played(modifiers, 60, 20) == std::pair{60, 40});
	CHECK(played(modifiers, 60, 100) == std::pair{60, 90});
	CHECK(played(modifiers, 60, 64) == std::pair{60, 64});

	modifiers.range[0] = {midi::RangeTarget::VelocityFilter, 40, 90};
	CHECK(played(modifiers, 60, 39) == std::pair{-1, -1});
	CHECK(played(modifiers, 60, 91) == std::pair{-1, -1});
	CHECK(played(modifiers, 60, 40) == std::pair{60, 40});

	modifiers.range[0] = {midi::RangeTarget::NoteLimit, 48, 72}; // C2 to C4
	CHECK(played(modifiers, 30, 100) == std::pair{54, 100});
	CHECK(played(modifiers, 90, 100) == std::pair{66, 100});
	CHECK(played(modifiers, 60, 100) == std::pair{60, 100});

	modifiers.range[0] = {midi::RangeTarget::NoteFilter, 48, 72};
	CHECK(played(modifiers, 47, 100) == std::pair{-1, -1});
	CHECK(played(modifiers, 73, 100) == std::pair{-1, -1});
	CHECK(played(modifiers, 72, 100) == std::pair{72, 100});

	// The second range works on what the first one left; both after transposing.
	modifiers.transpose = 12;
	CHECK(played(modifiers, 61, 100) == std::pair{-1, -1});
	modifiers.range[1] = {midi::RangeTarget::VelocityLimit, 0, 50};
	CHECK(played(modifiers, 50, 100) == std::pair{62, 50});
}

TEST_CASE("bypass plays the notes as they are", "[unit]")
{
	midi::Modifiers modifiers;
	modifiers.transpose = 5;
	modifiers.velocityShift = 20;
	modifiers.range[0] = {midi::RangeTarget::NoteFilter, 0, 10};
	modifiers.random[0] = {midi::RandomTarget::Pitch, 3, 3};
	modifiers.bypass = true;
	CHECK(played(modifiers, 60, 100) == std::pair{60, 100});
	// Velocities aren't touched while no velocity modifier is on, so their 16 bits stay.
	modifiers.bypass = false;
	modifiers.range = {};
	modifiers.velocityShift = 0;
	uint8_t key = 60;
	uint16_t value = 12345;
	CHECK(midi::modifyNote(modifiers, 1, key, value));
	CHECK(key == 68);
	CHECK(value == 12345);
}

TEST_CASE("channel.set changes a track's MIDI modifiers, which project.get shows and undo restores", "[integration]")
{
	ModifiedTrack track;
	const Json defaults = run(track.client, "project.get").at("channels")[0].at("modifiers");
	CHECK(defaults ==
		Json{{"bypass", false}, {"transpose", 0}, {"velocityShift", 0}, {"velocityCompression", {1, 1}},
			{"lengthCompression", {1, 1}},
			{"random",
				{{{"target", "off"}, {"minimum", 0}, {"maximum", 0}},
					{{"target", "off"}, {"minimum", 0}, {"maximum", 0}}}},
			{"range",
				{{{"target", "off"}, {"minimum", 0}, {"maximum", 127}},
					{{"target", "off"}, {"minimum", 0}, {"maximum", 127}}}}});

	Json set = track.modify({{"transpose", 3}, {"random", {Json::object(), {{"target", "pitch"}, {"maximum", 2}}}}});
	CHECK(set.at("transpose") == 3);
	CHECK(set.at("random")[1] == Json{{"target", "pitch"}, {"minimum", 0}, {"maximum", 2}});
	// Only what it has changes.
	set = track.modify({{"velocityShift", -5}, {"range", {{{"target", "noteLimit"}, {"minimum", 36}}}}});
	CHECK(set.at("transpose") == 3);
	CHECK(set.at("velocityShift") == -5);
	CHECK(set.at("range")[0] == Json{{"target", "noteLimit"}, {"minimum", 36}, {"maximum", 127}});
	CHECK(run(track.client, "project.get").at("channels")[0].at("modifiers") == set);

	run(track.client, "history.undo");
	CHECK(run(track.client, "project.get").at("channels")[0].at("modifiers").at("velocityShift") == 0);
	run(track.client, "history.undo");
	CHECK(run(track.client, "project.get").at("channels")[0].at("modifiers") == defaults);
}

TEST_CASE("channel.set refuses MIDI modifiers out of range and on tracks without notes", "[integration]")
{
	ModifiedTrack track;
	const auto refused = [&track](Json modifiers)
	{ return reject(track.client, "channel.set", {{"channel", track.channel}, {"modifiers", std::move(modifiers)}}); };
	CHECK_THAT(refused({{"transpose", 128}}), ContainsSubstring("modifiers.transpose"));
	CHECK_THAT(refused({{"velocityShift", 1.5}}), ContainsSubstring("modifiers.velocityShift"));
	CHECK_THAT(refused({{"velocityCompression", {0, 1}}}), ContainsSubstring("from 1 to 16"));
	CHECK_THAT(refused({{"lengthCompression", {1, 17}}}), ContainsSubstring("from 1 to 16"));
	CHECK_THAT(refused({{"random", {{{"target", "tempo"}}}}}), ContainsSubstring("modifiers.random[0].target"));
	CHECK_THAT(refused({{"random", {{{"target", "pitch"}, {"minimum", 3}, {"maximum", 2}}}}}),
		ContainsSubstring("not be above"));
	CHECK_THAT(refused({{"random", {{{"target", "velocity"}, {"maximum", 128}}}}}), ContainsSubstring("-127 to 127"));
	CHECK_THAT(refused({{"random", {Json::object(), Json::object(), Json::object()}}}), ContainsSubstring("two"));
	CHECK_THAT(refused({{"range", {{{"target", "noteFilter"}, {"minimum", 80}, {"maximum", 70}}}}}),
		ContainsSubstring("not be above"));
	CHECK_THAT(refused({{"range", {{{"maximum", 128}}}}}), ContainsSubstring("0 to 127"));
	CHECK_THAT(refused(5), ContainsSubstring("modifiers must be object"));
	// Positions and lengths go up to a quarter note.
	CHECK(track.modify({{"random", {{{"target", "position"}, {"minimum", -g_beatTicks}}}}})
			  .at("random")[0]
			  .at("minimum") == -g_beatTicks);
	CHECK_THAT(refused({{"random", {{{"minimum", -g_beatTicks - 1}}}}}), ContainsSubstring("modifiers.random[0]"));
	// A target with smaller limits keeps what fits.
	CHECK(track.modify({{"random", {{{"target", "pitch"}}}}}).at("random")[0].at("minimum") == -127);

	const ObjectId audio = run(track.client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
	CHECK_THAT(reject(track.client, "channel.set", {{"channel", audio}, {"modifiers", {{"transpose", 1}}}}),
		ContainsSubstring("only MIDI and instrument tracks"));
	const ObjectId midi = run(track.client, "track.add", {{"type", "midi"}}).at("channel").at("id").get<ObjectId>();
	CHECK(run(track.client, "channel.set", {{"channel", midi}, {"modifiers", {{"transpose", 1}}}})
			  .at("channel")
			  .at("modifiers")
			  .at("transpose") == 1);
}

TEST_CASE(
	"the modifiers play the part's notes transposed, shifted and compressed, without changing them", "[integration]")
{
	ModifiedTrack track;
	track.add({{60, 100}, {62, 40}, {64, 127}});
	CHECK(pitches(track.notes()) == std::vector<int>{60, 62, 64});

	track.modify({{"transpose", -2}, {"velocityShift", 10}});
	CHECK(pitches(track.notes()) == std::vector<int>{58, 60, 62});
	CHECK(velocities(track.notes()) == std::vector<int>{110, 50, 127});
	track.modify({{"velocityCompression", {1, 2}}});
	CHECK(velocities(track.notes()) == std::vector<int>{60, 30, 74});
	// The notes in the part stay as they are.
	const Json part = run(track.client, "project.get").at("midiParts")[0];
	CHECK(part.at("notes")[0].at("pitch") == 60);

	track.modify({{"lengthCompression", {3, 1}}});
	CHECK(track.notes()[0].end - track.notes()[0].start == g_beat * 3 / 2);
	// Notes stay within their part.
	track.add({{70, 100}, {70, 100}, {70, 100}, {70, 100}});
	CHECK(track.notes().back().start == 3 * g_beat);
	CHECK(track.notes().back().end == 4 * g_beat);

	track.modify({{"bypass", true}});
	CHECK(pitches(track.notes()) == std::vector<int>{60, 70, 62, 70, 64, 70, 70});
	CHECK(track.notes()[0].end - track.notes()[0].start == g_beat / 2);
}

TEST_CASE("random modifiers vary each note of a part, the same on every playback", "[integration]")
{
	ModifiedTrack track;
	std::vector<std::pair<int, int>> notes(4, {60, 64});
	track.add(notes);
	track.add(notes);
	track.modify({{"random",
		{{{"target", "position"}, {"minimum", -g_beatTicks / 4}, {"maximum", g_beatTicks / 4}},
			{{"target", "length"}, {"minimum", 0}, {"maximum", g_beatTicks / 4}}}}});
	const std::vector<RenderNote> first = track.notes();
	REQUIRE(first.size() == 8);
	std::set<int64_t> starts;
	for (size_t i = 0; i < first.size(); ++i)
	{
		const int64_t beat = (first[i].start + (g_beat / 2)) / g_beat * g_beat;
		CHECK(first[i].start >= std::max<int64_t>(0, beat - (g_beat / 4)));
		CHECK(first[i].start <= beat + (g_beat / 4));
		const int64_t length = first[i].end - first[i].start;
		CHECK(length >= g_beat / 2);
		CHECK(length <= (g_beat / 2) + (g_beat / 4));
		starts.insert(first[i].start);
	}
	CHECK(starts.size() > 4); // the notes on one beat vary too

	// Another change compiles them again with the same values.
	track.modify({{"transpose", 0}});
	const std::vector<RenderNote> again = track.notes();
	for (size_t i = 0; i < first.size(); ++i)
	{
		CHECK(again[i].start == first[i].start);
		CHECK(again[i].end == first[i].end);
	}

	track.modify({{"random",
		{{{"target", "velocity"}, {"minimum", -20}, {"maximum", 20}},
			{{"target", "pitch"}, {"minimum", 0}, {"maximum", 2}}}}});
	for (const RenderNote& note : track.notes())
	{
		CHECK(note.pitch >= 60);
		CHECK(note.pitch <= 62);
		CHECK(midi::velocityToMidi1(note.velocity) >= 44);
		CHECK(midi::velocityToMidi1(note.velocity) <= 84);
	}
}

TEST_CASE("ranges filter and limit the part's notes", "[integration]")
{
	ModifiedTrack track;
	track.add({{30, 20}, {60, 64}, {90, 120}});
	track.modify({{"range", {{{"target", "noteFilter"}, {"minimum", 48}, {"maximum", 72}}}}});
	CHECK(pitches(track.notes()) == std::vector<int>{60});
	track.modify({{"range", {{{"target", "noteLimit"}}}}});
	CHECK(pitches(track.notes()) == std::vector<int>{54, 60, 66});
	track.modify({{"range", {{{"target", "velocityFilter"}, {"minimum", 21}, {"maximum", 119}}}}});
	CHECK(velocities(track.notes()) == std::vector<int>{64});
	track.modify({{"range", {{{"target", "velocityLimit"}}}}});
	CHECK(velocities(track.notes()) == std::vector<int>{21, 64, 119});
	track.modify({{"range", {{{"target", "off"}}}}});
	CHECK(pitches(track.notes()) == std::vector<int>{30, 60, 90});
}

TEST_CASE(
	"the modifiers play the notes played into a track live, each note-off where its note-on went", "[integration]")
{
	ModifiedTrack track;
	track.modify({{"transpose", 12}, {"velocityShift", -20}, {"range", {{{"target", "noteFilter"}, {"maximum", 90}}}}});
	run(track.client, "midi.capture", {{"start", true}});
	const auto message = [](int64_t position, std::vector<int> bytes)
	{ return Json{{"position", position}, {"bytes", std::move(bytes)}}; };
	run(track.client, "midi.inject",
		{{"channel", track.channel},
			{"messages", {message(10, {0x90, 60, 100}), message(20, {0x90, 80, 100}), message(30, {0xA0, 60, 50})}}});
	run(track.client, "audio.render", {{"frames", 64}});
	// The transpose changes while the note sounds: its note-off still goes to the note it started.
	track.modify({{"transpose", 5}});
	run(track.client, "midi.inject",
		{{"channel", track.channel}, {"messages", {message(100, {0x80, 60, 64}), message(110, {0x80, 80, 64})}}});
	run(track.client, "audio.render", {{"frames", 128}});

	std::vector<std::vector<int64_t>> captured;
	const Json capture = run(track.client, "midi.capture");
	for (const Json& c : capture.at("captured"))
	{
		std::vector<int64_t> bytes = c.at("bytes").get<std::vector<int64_t>>();
		bytes.insert(bytes.begin(), c.at("position").get<int64_t>());
		captured.push_back(bytes);
	}
	// 80 + 12 is beyond the range: neither its note-on nor its note-off plays.
	CHECK(captured == std::vector<std::vector<int64_t>>{{10, 0x90, 72, 80}, {30, 0xA0, 72, 50}, {100, 0x80, 72, 64}});
}

TEST_CASE("the MIDI modifiers are saved with the project", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "song.dawproject").u8string();
	ModifiedTrack track;
	const Json set = track.modify({{"bypass", true}, {"transpose", -7}, {"lengthCompression", {5, 4}},
		{"random", {{{"target", "length"}, {"minimum", -10}, {"maximum", 10}}}},
		{"range", {Json::object(), {{"target", "velocityFilter"}, {"minimum", 1}}}}});
	run(track.client, "midiPart.set", {{"part", track.part}, {"transpose", 4}, {"velocity", -9}});
	run(track.client, "project.save", {{"path", file}});
	run(track.client, "project.new");
	run(track.client, "project.load", {{"path", file}});
	const Json project = run(track.client, "project.get");
	CHECK(project.at("channels")[0].at("modifiers") == set);
	CHECK(project.at("midiParts")[0].at("transpose") == 4);
	CHECK(project.at("midiParts")[0].at("velocity") == -9);
}

TEST_CASE("a part's transpose and velocity add to the track's modifiers, also while they are bypassed", "[integration]")
{
	ModifiedTrack track;
	track.add({{60, 100}, {64, 50}});
	const Json part =
		run(track.client, "midiPart.set", {{"part", track.part}, {"transpose", 2}, {"velocity", 10}}).at("part");
	CHECK(part.at("transpose") == 2);
	CHECK(part.at("velocity") == 10);
	CHECK(pitches(track.notes()) == std::vector<int>{62, 66});
	CHECK(velocities(track.notes()) == std::vector<int>{110, 60});
	track.modify({{"transpose", 12}, {"velocityShift", -30}});
	CHECK(pitches(track.notes()) == std::vector<int>{74, 78});
	CHECK(velocities(track.notes()) == std::vector<int>{80, 30});
	track.modify({{"bypass", true}});
	CHECK(pitches(track.notes()) == std::vector<int>{62, 66});
	CHECK(velocities(track.notes()) == std::vector<int>{110, 60});

	run(track.client, "history.undo");
	run(track.client, "history.undo");
	run(track.client, "history.undo");
	CHECK(pitches(track.notes()) == std::vector<int>{60, 64});
	CHECK(run(track.client, "project.get").at("midiParts")[0].at("transpose") == 0);
	CHECK_THAT(reject(track.client, "midiPart.set", {{"part", track.part}, {"transpose", 128}}),
		ContainsSubstring("transpose"));
	CHECK_THAT(reject(track.client, "midiPart.set", {{"part", track.part}, {"velocity", -128}}),
		ContainsSubstring("velocity"));
}
