// SPDX-License-Identifier: GPL-3.0-or-later
// Editing parts and events (#85): the event.* commands on audio events and MIDI parts, shared copies and locks; and
// range editing (#88), the range.* commands on audio, MIDI and automation.
// tests/eventediting_tests.cpp drives the same commands through the event display's gestures.
#include "engine/document/projectmodel.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using gnubase::engine::Client;
using gnubase::engine::Engine;
using gnubase::engine::Json;

namespace
{
	constexpr int64_t g_quarter = gnubase::time::g_ticksPerQuarter;
	constexpr int64_t g_bar = 4 * g_quarter;
	constexpr int64_t g_second = 48000; // a quarter is half a second at 120 BPM

	struct Fixture
	{
		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			const gnubase::engine::Result result = client.call(command, parameters);
			INFO(command << ": " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		std::string fails(const std::string& command, const Json& parameters)
		{
			const gnubase::engine::Result result = client.call(command, parameters);
			// Not REQUIRE_FALSE: Catch2's Normal | FalseTest trips clang-analyzer's EnumCastOutOfRange check
			REQUIRE(!result.ok());
			return result.error;
		}

		int64_t track(const char* type, const char* timeBase = "musical")
		{
			return call("track.add", {{"type", type}, {"timeBase", timeBase}}).at("track").at("id").get<int64_t>();
		}

		// An audio event at second 1 playing seconds 2 to 4 of a 10 second clip, on a linear audio track.
		int64_t audioEvent(int64_t onTrack)
		{
			const Json media = call(
				"media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 10 * g_second}})
								   .at("media");
			const Json clip = call("clip.add", {{"media", media.at("id")}}).at("clip");
			return call("audioEvent.add",
				{{"track", onTrack}, {"clip", clip.at("id")}, {"position", g_second}, {"offset", 2 * g_second},
					{"length", 2 * g_second}})
				.at("event")
				.at("id")
				.get<int64_t>();
		}

		// A one-bar part at bar 2 with a note on every quarter.
		int64_t part(int64_t onTrack)
		{
			const int64_t id = call("midiPart.add", {{"track", onTrack}, {"position", g_bar}, {"length", g_bar}})
								   .at("part")
								   .at("id")
								   .get<int64_t>();
			Json notes = Json::array();
			for (int64_t i = 0; i < 4; ++i)
				notes.push_back(
					{{"position", i * g_quarter}, {"length", g_quarter}, {"pitch", 60 + i}, {"velocity", 1.0}});
			call("note.add", {{"part", id}, {"notes", notes}});
			return id;
		}

		Json object(const char* kind, const Json& id)
		{
			const Json project = call("project.get");
			for (const Json& item : project.at(kind))
				if (item.at("id") == id)
					return item;
			FAIL("no " << kind << " " << id);
		}

		Json audio(const Json& id) { return object("audioEvents", id); }
		Json midi(const Json& id) { return object("midiParts", id); }

		size_t count(const char* kind) { return call("project.get").at(kind).size(); }

		Engine engine;
		Client client{engine};
	};
}

TEST_CASE("event.move moves events and parts by samples or ticks, onto tracks of their type", "[unit]")
{
	Fixture f;
	const int64_t audioTrack = f.track("audio", "linear");
	const int64_t otherAudio = f.track("audio");
	const int64_t midiTrack = f.track("midi");
	const int64_t event = f.audioEvent(audioTrack);
	const int64_t part = f.part(midiTrack);

	// Both by half a second: samples on the linear track, the same time in ticks on the musical one.
	f.call("event.move", {{"events", {event, part}}, {"delta", g_second / 2}});
	CHECK(f.audio(event).at("position") == g_second + (g_second / 2));
	CHECK(f.midi(part).at("position") == g_bar + g_quarter);
	CHECK(f.midi(part).at("length") == g_bar);

	// Ticks are exact; a step back undoes the move as one step.
	f.call("event.move", {{"events", {part}}, {"ticks", -g_quarter}});
	CHECK(f.midi(part).at("position") == g_bar);
	f.call("history.undo");
	CHECK(f.midi(part).at("position") == g_bar + g_quarter);

	// Onto another audio track, which counts in ticks.
	f.call("event.move", {{"events", {event}}, {"track", otherAudio}});
	CHECK(f.audio(event).at("track") == otherAudio);
	CHECK(f.audio(event).at("position") == 3 * g_quarter);

	CHECK_THAT(f.fails("event.move", {{"events", {part}}, {"track", audioTrack}}),
		ContainsSubstring("MIDI or instrument track"));
	CHECK_THAT(f.fails("event.move", {{"events", {part}}, {"delta", -10 * g_second}}),
		ContainsSubstring("before the project start"));
	CHECK_THAT(f.fails("event.move", {{"events", {part}}, {"delta", 1}, {"ticks", 1}}), ContainsSubstring("not both"));
	CHECK_THAT(f.fails("event.move", {{"events", {12345}}}), ContainsSubstring("no audio event or part"));

	// Locked events stay where they are, and a failed command changes nothing.
	f.call("event.set", {{"events", {event}}, {"lock", true}});
	CHECK(f.audio(event).at("lock") == true);
	CHECK_THAT(f.fails("event.move", {{"events", {part, event}}, {"delta", g_second}}), ContainsSubstring("locked"));
	CHECK(f.midi(part).at("position") == g_bar + g_quarter);
}

TEST_CASE("event.copy and event.duplicate make real and shared copies", "[unit]")
{
	Fixture f;
	const int64_t audioTrack = f.track("audio", "linear");
	const int64_t midiTrack = f.track("instrument");
	const int64_t event = f.audioEvent(audioTrack);
	const int64_t part = f.part(midiTrack);

	// A copy of the audio event plays the same clip; a real copy of the part has notes of its own.
	const Json copies = f.call("event.copy", {{"events", {event, part}}, {"ticks", 2 * g_bar}}).at("events");
	REQUIRE(copies.size() == 2);
	CHECK(f.audio(copies[0]).at("clip") == f.audio(event).at("clip"));
	CHECK(f.audio(copies[0]).at("position") == g_second + (4 * g_second));
	const Json realCopy = f.midi(copies[1]);
	CHECK(realCopy.at("position") == 3 * g_bar);
	CHECK(realCopy.at("shared") == 0);
	REQUIRE(realCopy.at("notes").size() == 4);
	CHECK(realCopy.at("notes")[0].at("id") != f.midi(part).at("notes")[0].at("id"));

	// Repeating three times: shared copies one bar apart, all in one group.
	const Json repeated = f.call("event.duplicate", {{"events", {part}}, {"count", 3}, {"shared", true}}).at("events");
	REQUIRE(repeated.size() == 3);
	for (int64_t i = 0; i < 3; ++i)
	{
		const Json copy = f.midi(repeated[i]);
		CHECK(copy.at("position") == g_bar * (i + 2));
		CHECK(copy.at("shared") != 0);
		CHECK(copy.at("shared") == f.midi(part).at("shared"));
	}

	// Editing one edits all of them; the real copy keeps its notes.
	const int64_t note = f.midi(repeated[1]).at("notes")[0].at("id").get<int64_t>();
	f.call("note.set", {{"part", repeated[1]}, {"notes", {{{"id", note}, {"pitch", 72}}}}});
	CHECK(f.midi(part).at("notes")[0].at("pitch") == 72);
	CHECK(f.midi(repeated[2]).at("notes")[0].at("pitch") == 72);
	CHECK(f.midi(copies[1]).at("notes")[0].at("pitch") == 60);
	f.call(
		"note.add", {{"part", part}, {"notes", {{{"position", 0}, {"length", 10}, {"pitch", 30}, {"velocity", 1.0}}}}});
	CHECK(f.midi(repeated[0]).at("notes").size() == 5);
	f.call("history.undo");
	CHECK(f.midi(repeated[0]).at("notes").size() == 4);

	// A real copy goes its own way; a last copy alone is a part like any other.
	f.call("event.toRealCopy", {{"events", {repeated[0], repeated[1]}}});
	CHECK(f.midi(repeated[0]).at("shared") == 0);
	f.call("event.remove", {{"events", {repeated[2]}}});
	CHECK(f.midi(part).at("shared") == 0);

	// An audio event's real copy plays a clip of its own.
	const size_t clips = f.count("clips");
	f.call("event.toRealCopy", {{"events", {copies[0]}}});
	CHECK(f.count("clips") == clips + 1);
	CHECK(f.audio(copies[0]).at("clip") != f.audio(event).at("clip"));

	// One frame of a 192 kHz clip is no samples long at 48 kHz: its copies would all lie on top of it.
	const Json media =
		f.call("media.add", {{"path", "fast.wav"}, {"sampleRate", 192000}, {"channels", 1}, {"length", 192000}})
			.at("media");
	const Json clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const Json tiny =
		f.call("audioEvent.add", {{"track", audioTrack}, {"clip", clip.at("id")}, {"position", 0}, {"length", 1}})
			.at("event")
			.at("id");
	const size_t events = f.count("audioEvents");
	CHECK_THAT(f.fails("event.duplicate", {{"events", {tiny}}, {"count", 3}}), ContainsSubstring("too short"));
	CHECK(f.count("audioEvents") == events);
}

TEST_CASE("A transaction keeps every shared copy's edit, as one undo step", "[unit]")
{
	Fixture f;
	const int64_t original = f.part(f.track("midi"));
	const int64_t copy = f.call("event.copy", {{"events", {original}}, {"ticks", g_bar}, {"shared", true}})
							 .at("events")[0]
							 .get<int64_t>();
	const Json notes = f.midi(original).at("notes");
	const Json added = {{"position", 0}, {"length", 10}, {"pitch", 30}, {"velocity", 1.0}};
	const auto edit = [](const char* command, int64_t part, const Json& changes)
	{ return Json{{"command", command}, {"parameters", {{"part", part}, {"notes", changes}}}}; };

	// Different notes in each copy, then a note added to one copy that the next command edits in the other.
	const Json result = f.call("history.transaction",
		{{"description", "Edit both"},
			{"commands",
				{edit("note.set", original, {{{"id", notes[0].at("id")}, {"pitch", 72}}}),
					edit("note.set", copy, {{{"id", notes[1].at("id")}, {"pitch", 73}}}),
					edit("note.add", copy, Json::array({added})),
					{{"command", "midi.fixVelocities"}, {"parameters", {{"parts", {original}}, {"velocity", 0.5}}}}}}});
	const Json newNote = result.at("results")[2].at("notes")[0].at("id");

	for (const int64_t part : {original, copy})
	{
		const Json now = f.midi(part).at("notes");
		REQUIRE(now.size() == 5);
		CHECK(now[0].at("pitch") == 72);
		CHECK(now[1].at("pitch") == 73);
		CHECK(now[4].at("id") == newNote);
		for (const Json& note : now)
			CHECK(note.at("velocity").get<double>() < 0.6);
	}
	CHECK(f.midi(original).at("notes") == f.midi(copy).at("notes"));

	// One step undoes both copies' edits.
	f.call("history.undo");
	CHECK(f.midi(original).at("notes") == notes);
	CHECK(f.midi(copy).at("notes") == notes);
}

TEST_CASE("A transaction syncs parts that become or stop being shared in it, and leaves other parts alone", "[unit]")
{
	Fixture f;
	const int64_t track = f.track("midi");
	const int64_t otherTrack = f.track("midi");
	const int64_t original = f.part(track);
	const int64_t alone =
		f.call("event.copy", {{"events", {original}}, {"ticks", 2 * g_bar}}).at("events")[0].get<int64_t>();
	const int64_t leaving = f.part(otherTrack);
	const int64_t staying = f.call("event.copy", {{"events", {leaving}}, {"ticks", 2 * g_bar}, {"shared", true}})
								.at("events")[0]
								.get<int64_t>();
	const Json notes = f.midi(original).at("notes");
	const Json aloneNotes = f.midi(alone).at("notes");
	const Json stayingNotes = f.midi(staying).at("notes");
	const auto setPitch = [](int64_t part, const Json& note, int pitch)
	{
		return Json{{"command", "note.set"},
			{"parameters", {{"part", part}, {"notes", {{{"id", note.at("id")}, {"pitch", pitch}}}}}}};
	};
	const auto pitches = [&f](int64_t part)
	{
		const Json midiPart = f.midi(part);
		std::vector<int> result;
		result.reserve(midiPart.at("notes").size());
		for (const Json& note : midiPart.at("notes"))
			result.push_back(note.at("pitch").get<int>());
		return result;
	};
	const Json before = f.call("project.get").at("midiParts");

	// The original's first edit is its own; it becomes shared, and its next edit reaches the new copy. Its real copy
	// keeps its notes and edits only itself. Once one of two copies is a real copy, the other edits only itself too.
	const Json result = f.call("history.transaction",
		{{"description", "Edit"},
			{"commands",
				{setPitch(original, notes[0], 70),
					{{"command", "event.copy"},
						{"parameters", {{"events", {original}}, {"ticks", 4 * g_bar}, {"shared", true}}}},
					setPitch(original, notes[1], 71), setPitch(alone, aloneNotes[2], 50),
					{{"command", "event.toRealCopy"}, {"parameters", {{"events", {leaving}}}}},
					setPitch(staying, stayingNotes[3], 40)}}});
	const int64_t copy = result.at("results")[1].at("events")[0].get<int64_t>();
	CHECK(f.midi(original).at("shared") != 0);
	CHECK(f.midi(copy).at("shared") == f.midi(original).at("shared"));
	CHECK(pitches(original) == std::vector<int>{70, 71, 62, 63});
	CHECK(f.midi(copy).at("notes") == f.midi(original).at("notes"));
	CHECK(f.midi(alone).at("shared") == 0);
	CHECK(pitches(alone) == std::vector<int>{60, 61, 50, 63});
	CHECK(f.midi(leaving).at("shared") == 0);
	CHECK(f.midi(staying).at("shared") == 0);
	CHECK(pitches(leaving) == std::vector<int>{60, 61, 62, 63});
	CHECK(pitches(staying) == std::vector<int>{60, 61, 62, 40});

	// The copy, which shares the original's note ids, edits the original in a transaction of its own.
	f.call("history.transaction", {{"description", "Edit the copy"}, {"commands", {setPitch(copy, notes[2], 72)}}});
	CHECK(pitches(original) == std::vector<int>{70, 71, 72, 63});
	CHECK(f.midi(copy).at("notes") == f.midi(original).at("notes"));
	CHECK(pitches(alone) == std::vector<int>{60, 61, 50, 63});

	// Each transaction is one undo step, and redo brings back the same parts and notes.
	const Json after = f.call("project.get").at("midiParts");
	f.call("history.undo");
	const Json afterFirst = f.call("project.get").at("midiParts");
	CHECK(pitches(original) == std::vector<int>{70, 71, 62, 63});
	CHECK(f.midi(copy).at("notes") == f.midi(original).at("notes"));
	f.call("history.undo");
	CHECK(f.call("project.get").at("midiParts") == before);
	f.call("history.redo");
	CHECK(f.call("project.get").at("midiParts") == afterFirst);
	f.call("history.redo");
	CHECK(f.call("project.get").at("midiParts") == after);
}

TEST_CASE("A shared copy left alone by a removal in a transaction edits only itself", "[unit]")
{
	Fixture f;
	const int64_t original = f.part(f.track("midi"));
	const int64_t copy = f.call("event.copy", {{"events", {original}}, {"ticks", g_bar}, {"shared", true}})
							 .at("events")[0]
							 .get<int64_t>();
	const Json notes = f.midi(original).at("notes");
	const Json before = f.call("project.get").at("midiParts");

	f.call("history.transaction",
		{{"description", "Remove and edit"},
			{"commands",
				{{{"command", "event.remove"}, {"parameters", {{"events", {original}}}}},
					{{"command", "note.set"},
						{"parameters", {{"part", copy}, {"notes", {{{"id", notes[0].at("id")}, {"pitch", 70}}}}}}}}}});
	CHECK(f.count("midiParts") == 1);
	CHECK(f.midi(copy).at("shared") == 0);
	CHECK(f.midi(copy).at("notes")[0].at("pitch") == 70);

	f.call("history.undo");
	CHECK(f.call("project.get").at("midiParts") == before);
}

TEST_CASE("Only shared parts are copied for comparing shared copies", "[unit]")
{
	using gnubase::engine::MidiPart;
	using gnubase::engine::ObjectId;

	// A dense part that isn't shared, beside a group of two copies.
	std::map<ObjectId, MidiPart> parts;
	MidiPart dense;
	dense.id = 1;
	for (ObjectId note = 1; note <= 1000; ++note)
		dense.notes[note].id = note;
	parts[dense.id] = dense;
	for (const ObjectId id : {ObjectId{2}, ObjectId{3}})
	{
		MidiPart copy;
		copy.id = id;
		copy.shared = 7;
		copy.notes[1].id = 1;
		parts[id] = copy;
	}

	const std::map<ObjectId, MidiPart> snapshot = gnubase::engine::sharedPartsOf(parts);
	REQUIRE(snapshot.size() == 2);
	CHECK(snapshot.count(1) == 0);
	CHECK(snapshot.at(2).shared == 7);
	CHECK(snapshot.at(3).notes.size() == 1);
	CHECK(gnubase::engine::sharedPartsOf({}).empty());
}

TEST_CASE("event.resize moves the start or the end, revealing content or moving it", "[unit]")
{
	Fixture f;
	const int64_t event = f.audioEvent(f.track("audio", "linear"));
	const int64_t part = f.part(f.track("midi"));

	// Normal sizing: the start half a second earlier reveals half a second more of the clip.
	f.call("event.resize", {{"events", {event}}, {"edge", "start"}, {"delta", -g_second / 2}});
	Json audio = f.audio(event);
	CHECK(audio.at("position") == g_second / 2);
	CHECK(audio.at("offset") == (2 * g_second) - (g_second / 2));
	CHECK(audio.at("length") == (2 * g_second) + (g_second / 2));

	// The end stops at the clip's end, the start at its beginning.
	f.call("event.resize", {{"events", {event}}, {"edge", "end"}, {"delta", 100 * g_second}});
	CHECK(f.audio(event).at("offset").get<int64_t>() + f.audio(event).at("length").get<int64_t>() == 10 * g_second);
	f.call("event.resize", {{"events", {event}}, {"edge", "start"}, {"delta", -100 * g_second}});
	CHECK(f.audio(event).at("position") == 0);
	CHECK(f.audio(event).at("offset") == (2 * g_second) - g_second);

	// Sizing moves contents: the offset stays at the start.
	const Json before = f.audio(event);
	f.call("event.resize", {{"events", {event}}, {"edge", "start"}, {"delta", g_second}, {"mode", "moveContents"}});
	CHECK(f.audio(event).at("offset") == before.at("offset"));
	CHECK(f.audio(event).at("position") == g_second);
	CHECK(f.audio(event).at("length") == before.at("length").get<int64_t>() - g_second);

	// A part's start a quarter later: its notes keep their time, the first one is outside and goes.
	f.call("event.resize", {{"events", {part}}, {"edge", "start"}, {"ticks", g_quarter}});
	Json midi = f.midi(part);
	CHECK(midi.at("position") == g_bar + g_quarter);
	CHECK(midi.at("length") == 3 * g_quarter);
	REQUIRE(midi.at("notes").size() == 3);
	CHECK(midi.at("notes")[0].at("position") == 0);
	CHECK(midi.at("notes")[0].at("pitch") == 61);

	// Its end a quarter later with the contents: the notes move with it.
	f.call("event.resize", {{"events", {part}}, {"edge", "end"}, {"ticks", g_quarter}, {"mode", "moveContents"}});
	midi = f.midi(part);
	CHECK(midi.at("length") == g_bar);
	CHECK(midi.at("notes")[0].at("position") == g_quarter);
}

TEST_CASE("event.split splits events and parts at positions", "[unit]")
{
	Fixture f;
	const int64_t audioTrack = f.track("audio", "linear");
	const int64_t event = f.audioEvent(audioTrack);
	const int64_t part = f.part(f.track("midi"));
	f.call("audioEvent.setFades", {{"event", event}, {"fadeIn", {{"length", 100}}}, {"fadeOut", {{"length", 200}}}});

	// At the cursor, at 2.25 seconds: everything there is cut (the part from 2 to 4 seconds, the event from 1 to 3).
	const Json added = f.call("event.split", {{"positions", {(9 * g_second) / 4}}}).at("events");
	REQUIRE(added.size() == 2);
	const Json left = f.audio(event);
	const Json right = f.audio(added[0]);
	CHECK(left.at("length") == (5 * g_second) / 4);
	CHECK(left.at("fadeIn").at("length") == 100);
	CHECK(left.at("fadeOut").at("length") == 0);
	CHECK(right.at("position") == (9 * g_second) / 4);
	CHECK(right.at("offset") == (2 * g_second) + ((5 * g_second) / 4));
	CHECK(right.at("length") == (3 * g_second) / 4);
	CHECK(right.at("fadeOut").at("length") == 200);

	// The part's note across the split stays in the first part; the others go by where they start.
	Json first = f.midi(part);
	const Json second = f.midi(added[1]);
	CHECK(first.at("length") == g_quarter / 2);
	CHECK(first.at("notes").size() == 1);
	CHECK(second.at("position") == g_bar + (g_quarter / 2));
	REQUIRE(second.at("notes").size() == 3);
	CHECK(second.at("notes")[0].at("position") == g_quarter / 2);
	f.call("history.undo");

	// With splitNotes the note is cut in two; split loop is a split at both locators.
	f.call(
		"event.split", {{"events", {part}}, {"positions", {(9 * g_second) / 4, 3 * g_second}}, {"splitNotes", true}});
	REQUIRE(f.count("midiParts") == 3);
	first = f.midi(part);
	REQUIRE(first.at("notes").size() == 1);
	CHECK(first.at("notes")[0].at("length") == g_quarter / 2);

	// Locked events aren't split unless named, and then refuse.
	f.call("event.set", {{"events", {event}}, {"lock", true}});
	CHECK(f.call("event.split", {{"positions", {(3 * g_second) / 2}}}).at("events").empty());
	CHECK_THAT(f.fails("event.split", {{"events", {event}}, {"positions", {(3 * g_second) / 2}}}),
		ContainsSubstring("locked"));
}

TEST_CASE("event.glue joins split audio events and MIDI parts", "[unit]")
{
	Fixture f;
	const int64_t audioTrack = f.track("audio", "linear");
	const int64_t event = f.audioEvent(audioTrack);
	const Json pieces = f.call("event.split", {{"positions", {(3 * g_second) / 2, 2 * g_second}}}).at("events");
	REQUIRE(pieces.size() == 2);

	f.call("event.glue", {{"events", {pieces[1], event, pieces[0]}}});
	CHECK(f.count("audioEvents") == 1);
	CHECK(f.audio(event).at("length") == 2 * g_second);
	CHECK(f.audio(event).at("offset") == 2 * g_second);

	// Audio events that don't continue each other glue into an audio part (tests/audiopart_tests.cpp).
	const Json copy = f.call("event.copy", {{"events", {event}}, {"delta", 3 * g_second}}).at("events");
	const Json audioPart = f.call("event.glue", {{"events", {event, copy[0]}}}).at("event");
	CHECK(f.audio(event).at("part") == audioPart);
	CHECK(f.audio(copy[0]).at("part") == audioPart);

	// Parts become one from the first start to the last end, with every note.
	const int64_t midiTrack = f.track("midi");
	const int64_t part = f.part(midiTrack);
	const Json later = f.call("event.copy", {{"events", {part}}, {"ticks", 2 * g_bar}}).at("events");
	f.call("event.glue", {{"events", {part, later[0]}}});
	const Json glued = f.midi(part);
	CHECK(glued.at("length") == 3 * g_bar);
	REQUIRE(glued.at("notes").size() == 8);
	CHECK(f.count("midiParts") == 1);
	CHECK_THAT(f.fails("event.glue", {{"events", {part, event}}}), ContainsSubstring("same track"));
}

TEST_CASE("event.set mutes and locks, event.remove deletes unlocked events", "[unit]")
{
	Fixture f;
	const int64_t event = f.audioEvent(f.track("audio", "linear"));
	const int64_t part = f.part(f.track("midi"));

	f.call("event.set", {{"events", {event, part}}, {"mute", true}});
	CHECK(f.audio(event).at("mute") == true);
	CHECK(f.midi(part).at("mute") == true);
	f.call("history.undo");
	CHECK(f.midi(part).at("mute") == false);
	CHECK(f.audio(event).at("mute") == false);
	f.call("history.redo");
	CHECK(f.midi(part).at("mute") == true);

	f.call("event.set", {{"events", {part}}, {"lock", true}});
	CHECK_THAT(f.fails("event.remove", {{"events", {part}}}), ContainsSubstring("locked"));
	CHECK_THAT(
		f.fails("event.resize", {{"events", {part}}, {"edge", "end"}, {"delta", 1}}), ContainsSubstring("locked"));
	f.call("event.set", {{"events", {part}}, {"lock", false}, {"mute", false}});
	CHECK(f.midi(part).at("mute") == false);

	f.call("event.remove", {{"events", {event, part}}});
	CHECK(f.count("audioEvents") == 0);
	CHECK(f.count("midiParts") == 0);
	f.call("history.undo");
	CHECK(f.count("midiParts") == 1);
}

TEST_CASE("event.set puts events on lanes, which splits, undo and the project file keep", "[integration]")
{
	const gnubase::test::TempDir dir;
	Fixture f;
	const int64_t event = f.audioEvent(f.track("audio", "linear"));
	const int64_t part = f.part(f.track("midi"));
	CHECK(f.midi(part).at("lane") == 0);

	f.call("event.set", {{"events", {event, part}}, {"lane", 2}});
	CHECK(f.audio(event).at("lane") == 2);
	CHECK(f.midi(part).at("lane") == 2);
	CHECK(f.call("event.get", {{"events", {part}}}).at("events").at(0).at("lane") == 2);
	f.call("history.undo");
	CHECK(f.midi(part).at("lane") == 0);
	CHECK(f.audio(event).at("lane") == 0);
	f.call("history.redo");
	CHECK(f.midi(part).at("lane") == 2);

	f.call("event.split", {{"events", {part}}, {"positions", {3 * g_second}}});
	REQUIRE(f.count("midiParts") == 2);
	const Json project = f.call("project.get");
	for (const Json& item : project.at("midiParts"))
		CHECK(item.at("lane") == 2);
	CHECK_THAT(f.fails("event.set", {{"events", {event}}, {"lane", -1}}), ContainsSubstring("lane"));

	const std::string file = (dir.path() / "lanes.dawproject").u8string();
	f.call("project.save", {{"path", file}, {"embedMedia", false}});
	f.call("project.new");
	f.call("project.load", {{"path", file}});
	CHECK(f.audio(event).at("lane") == 2);
	CHECK(f.midi(part).at("lane") == 2);
}

TEST_CASE("event.fillLoop fills the locators with copies, cutting the last one", "[unit]")
{
	Fixture f;
	const int64_t part = f.part(f.track("midi"));
	// Locators from bar 2 to the middle of bar 5: copies at bars 3 and 4, and half a bar at bar 5.
	f.call("transport.set", {{"left", 2 * g_second}, {"right", 9 * g_second}});
	const Json copies = f.call("event.fillLoop", {{"events", {part}}}).at("events");
	REQUIRE(copies.size() == 3);
	CHECK(f.midi(copies[0]).at("position") == 2 * g_bar);
	CHECK(f.midi(copies[1]).at("position") == 3 * g_bar);
	CHECK(f.midi(copies[2]).at("position") == 4 * g_bar);
	CHECK(f.midi(copies[2]).at("length") == g_bar / 2);
	CHECK(f.midi(copies[2]).at("notes").size() == 4); // cut by the part's end, not removed

	// A range of its own, with shared copies.
	const Json shared =
		f.call("event.fillLoop", {{"events", {part}}, {"start", 0}, {"end", 2 * g_second}, {"shared", true}})
			.at("events");
	REQUIRE(shared.size() == 1);
	CHECK(f.midi(shared[0]).at("position") == 0);
	CHECK(f.midi(shared[0]).at("shared") == f.midi(part).at("shared"));

	// One frame of a 192 kHz clip is no samples long at 48 kHz: nothing to fill the range with.
	const Json media =
		f.call("media.add", {{"path", "fast.wav"}, {"sampleRate", 192000}, {"channels", 1}, {"length", 192000}})
			.at("media");
	const Json clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const Json tiny =
		f.call("audioEvent.add",
			 {{"track", f.track("audio", "linear")}, {"clip", clip.at("id")}, {"position", 0}, {"length", 1}})
			.at("event")
			.at("id");
	CHECK(f.fails("event.fillLoop", {{"events", {tiny}}, {"start", 0}, {"end", g_second}}).find("too short") !=
		std::string::npos);
}

TEST_CASE("event.get tells where events and parts are, in samples and ticks", "[unit]")
{
	Fixture f;
	const int64_t event = f.audioEvent(f.track("audio", "linear"));
	const int64_t part = f.part(f.track("midi"));
	f.call("event.set", {{"events", {part}}, {"mute", true}});

	const Json info = f.call("event.get", {{"events", {event, part}}}).at("events");
	REQUIRE(info.size() == 2);
	CHECK(info[0].at("kind") == "audio");
	CHECK(info[0].at("name") == "take");
	CHECK(info[0].at("start") == g_second);
	CHECK(info[0].at("end") == 3 * g_second);
	CHECK(info[0].at("startTicks") == 2 * g_quarter);
	CHECK(info[1].at("kind") == "midi");
	CHECK(info[1].at("length") == 2 * g_second);
	CHECK(info[1].at("endTicks") == 2 * g_bar);
	CHECK(info[1].at("mute") == true);
	CHECK(info[1].at("lock") == false);
}

namespace
{
	using Spans = std::vector<std::pair<int64_t, int64_t>>;
	using Points = std::vector<std::pair<int64_t, double>>;
	constexpr int64_t g_start = (5 * g_second) / 2; // the range's start and end
	constexpr int64_t g_end = (7 * g_second) / 2;

	// Range editing (#88): an audio event from 1 to 3 seconds on a linear audio track with pan automation (-1 at 0,
	// 0.5 at 3 seconds and 1 at 4 seconds), and a part from 2 to 4 seconds with a note on every quarter on a MIDI track
	// with modulation automation (0 at the start, 1 at bar 3). The range is 2.5 to 3.5 seconds on both tracks.
	struct RangeFixture : Fixture
	{
		RangeFixture()
			: audioTrack(track("audio", "linear")), midiTrack(track("midi")), event(audioEvent(audioTrack)),
			  midiPart(part(midiTrack)),
			  pan(lane(audioTrack, {{"type", "pan"}}, {{0, -1.0}, {3 * g_second, 0.5}, {4 * g_second, 1.0}})),
			  modulation(lane(midiTrack, {{"type", "controller"}, {"number", 1}}, {{0, 0.0}, {2 * g_bar, 1.0}}))
		{
		}

		int64_t lane(int64_t onTrack, const Json& target, const Points& points)
		{
			const auto id =
				call("automationLane.add", {{"track", onTrack}, {"target", target}}).at("lane").at("id").get<int64_t>();
			Json list = Json::array();
			for (const auto& [position, value] : points)
				list.push_back({{"position", position}, {"value", value}});
			call("automationPoint.add", {{"lane", id}, {"points", list}});
			return id;
		}

		// A lane's points as position and value, in order.
		void checkPoints(int64_t laneId, const Points& expected)
		{
			Points actual;
			const Json lanes = call("automationLane.list");
			for (const Json& lane : lanes.at("lanes"))
				if (lane.at("id") == laneId)
					for (const Json& point : lane.at("points"))
						actual.emplace_back(point.at("position").get<int64_t>(), point.at("value").get<double>());
			REQUIRE(actual.size() == expected.size());
			for (size_t i = 0; i < expected.size(); ++i)
			{
				INFO("point " << i);
				CHECK(actual[i].first == expected[i].first);
				CHECK_THAT(actual[i].second, WithinAbs(expected[i].second, 1e-9));
			}
		}

		// The events and parts on a track as start and end in project samples, by start.
		Spans spans(int64_t onTrack)
		{
			Json ids = Json::array();
			const Json project = call("project.get");
			for (const char* kind : {"audioEvents", "midiParts"})
				for (const Json& item : project.at(kind))
					if (item.at("track") == onTrack)
						ids.push_back(item.at("id"));
			Spans result;
			if (ids.empty())
				return result;
			const Json info = call("event.get", {{"events", ids}});
			for (const Json& each : info.at("events"))
				result.emplace_back(each.at("start").get<int64_t>(), each.at("end").get<int64_t>());
			std::sort(result.begin(), result.end());
			return result;
		}

		Json range(const Json& more = Json::object()) const
		{
			Json parameters = {{"start", g_start}, {"end", g_end}, {"tracks", {audioTrack, midiTrack}}};
			parameters.update(more);
			return parameters;
		}

		size_t steps() { return call("history.get").at("steps").size(); }

		int64_t audioTrack;
		int64_t midiTrack;
		int64_t event;
		int64_t midiPart;
		int64_t pan;
		int64_t modulation;
	};
}

TEST_CASE("range.split cuts the events and parts on the range's tracks at its edges", "[unit]")
{
	RangeFixture f;
	const int64_t other = f.audioEvent(f.track("audio", "linear")); // on a track outside the range
	const Json added = f.call("range.split", f.range()).at("events");
	CHECK(added.size() == 3); // the event at the start, the part at both edges
	CHECK(f.spans(f.audioTrack) == Spans{{g_second, g_start}, {g_start, 3 * g_second}});
	CHECK(f.spans(f.midiTrack) == Spans{{2 * g_second, g_start}, {g_start, g_end}, {g_end, 4 * g_second}});
	CHECK(f.audio(other).at("length") == 2 * g_second);

	// A range of no length splits at its start; locked events stay whole.
	f.call("history.undo");
	f.call("event.set", {{"events", {f.event}}, {"lock", true}});
	CHECK(f.call("range.split", f.range({{"end", g_start}})).at("events").size() == 1);
	CHECK(f.spans(f.audioTrack) == Spans{{g_second, 3 * g_second}});
}

TEST_CASE("range.delete removes what is in the range, and with time closes the gap", "[unit]")
{
	RangeFixture f;
	f.call("range.delete", f.range());
	CHECK(f.spans(f.audioTrack) == Spans{{g_second, g_start}});
	CHECK(f.spans(f.midiTrack) == Spans{{2 * g_second, g_start}, {g_end, 4 * g_second}});
	// The automation outside the range keeps its curve: the value at the start holds until the end, where the curve
	// goes on from its value there. The modulation has no point in the range: nothing to delete there.
	f.checkPoints(f.pan, {{0, -1.0}, {g_start, 0.25}, {g_end, 0.75}, {4 * g_second, 1.0}});
	f.checkPoints(f.modulation, {{0, 0.0}, {2 * g_bar, 1.0}});
	f.call("history.undo");
	CHECK(f.spans(f.midiTrack) == Spans{{2 * g_second, 4 * g_second}});

	// Delete Time: what comes after moves back by the range's length, in ticks on the musical track.
	f.call("range.delete", f.range({{"time", true}}));
	CHECK(f.spans(f.audioTrack) == Spans{{g_second, g_start}});
	CHECK(f.spans(f.midiTrack) == Spans{{2 * g_second, g_start}, {g_start, 3 * g_second}});
	f.checkPoints(f.pan, {{0, -1.0}, {g_start, 0.25}, {g_start, 0.75}, {3 * g_second, 1.0}});
	f.checkPoints(f.modulation,
		{{0, 0.0}, {g_bar + g_quarter, 0.625}, {g_bar + g_quarter, 0.875}, {(2 * g_bar) - (2 * g_quarter), 1.0}});
	// The part after the range kept its last note.
	const Json parts = f.call("project.get").at("midiParts");
	REQUIRE(parts.size() == 2);
	CHECK(parts[1].at("position") == g_bar + g_quarter);
	REQUIRE(parts[1].at("notes").size() == 1);
	CHECK(parts[1].at("notes")[0].at("pitch") == 63);

	CHECK_THAT(f.fails("range.delete", f.range({{"end", g_start}})), ContainsSubstring("empty"));
	CHECK_THAT(f.fails("range.delete", f.range({{"tracks", {12345}}})), ContainsSubstring("no track"));
}

TEST_CASE("range.insertSilence makes room at the range's start as long as the range", "[unit]")
{
	RangeFixture f;
	f.call("range.insertSilence", f.range());
	CHECK(f.spans(f.audioTrack) == Spans{{g_second, g_start}, {g_end, 4 * g_second}});
	CHECK(f.spans(f.midiTrack) == Spans{{2 * g_second, g_start}, {g_end, 5 * g_second}});
	// The value at the start holds over the silence, and the curve goes on after it.
	f.checkPoints(f.pan, {{0, -1.0}, {g_start, 0.25}, {g_end, 0.25}, {4 * g_second, 0.5}, {5 * g_second, 1.0}});
	f.checkPoints(f.modulation,
		{{0, 0.0}, {g_bar + g_quarter, 0.625}, {g_bar + (3 * g_quarter), 0.625}, {(2 * g_bar) + (2 * g_quarter), 1.0}});
}

TEST_CASE("range.crop cuts the events that cross the range's edges to it", "[unit]")
{
	RangeFixture f;
	const int64_t inside = f.call("midiPart.add", {{"track", f.midiTrack}, {"position", 5 * g_bar}, {"length", g_bar}})
							   .at("part")
							   .at("id")
							   .get<int64_t>();
	const Json cropped = f.call("range.crop", f.range({{"end", 12 * g_second}})).at("events");
	REQUIRE(cropped.size() == 2);
	CHECK(f.spans(f.audioTrack) == Spans{{g_start, 3 * g_second}});
	CHECK(f.spans(f.midiTrack) == Spans{{g_start, 4 * g_second}, {10 * g_second, 12 * g_second}});
	CHECK(f.midi(inside).at("length") == g_bar); // inside the range: unaffected
	// The cropped audio plays on from where it was cut.
	CHECK(f.audio(cropped[0]).at("offset") == (2 * g_second) + ((3 * g_second) / 2));

	// With splitNotes, notes across an edge are cut instead of left out.
	f.call("history.undo");
	f.call("range.crop", f.range({{"start", (9 * g_second) / 4}, {"splitNotes", true}}));
	const Json parts = f.call("project.get").at("midiParts");
	const auto first = std::find_if(
		parts.begin(), parts.end(), [](const Json& part) { return part.at("position") == g_bar + (g_quarter / 2); });
	REQUIRE(first != parts.end());
	REQUIRE(first->at("notes").size() == 3);
	CHECK(first->at("notes")[0].at("length") == g_quarter / 2);
}

TEST_CASE("range.copy and range.cut keep the range for range.paste, which can paste time", "[unit]")
{
	RangeFixture f;
	CHECK_THAT(f.fails("range.paste", Json::object()), ContainsSubstring("nothing"));

	// Copying changes nothing and records no undo step.
	const size_t steps = f.steps();
	const Json copied = f.call("range.copy", f.range());
	CHECK(copied.at("events") == 2);
	CHECK(copied.at("lanes") == 2);
	CHECK(f.steps() == steps);
	CHECK(f.spans(f.audioTrack) == Spans{{g_second, 3 * g_second}});

	// Pasted at 4 seconds onto the tracks it came from: the piece of the event plays the same audio, the piece of the
	// part has its notes, the automation in the range replaces the curve there.
	const Json pasted = f.call("range.paste", {{"position", 4 * g_second}}).at("events");
	REQUIRE(pasted.size() == 2);
	CHECK(f.audio(pasted[0]).at("position") == 4 * g_second);
	CHECK(f.audio(pasted[0]).at("offset") == (2 * g_second) + ((3 * g_second) / 2));
	CHECK(f.audio(pasted[0]).at("length") == g_second / 2);
	const Json part = f.midi(pasted[1]);
	CHECK(part.at("position") == 2 * g_bar);
	CHECK(part.at("length") == 2 * g_quarter);
	REQUIRE(part.at("notes").size() == 2);
	CHECK(part.at("notes")[0].at("pitch") == 61);
	f.checkPoints(f.pan,
		{{0, -1.0}, {3 * g_second, 0.5}, {4 * g_second, 1.0}, {4 * g_second, 0.25}, {(9 * g_second) / 2, 0.5},
			{5 * g_second, 0.75}, {5 * g_second, 1.0}});

	// Cut Time takes the range out and closes the gap; Paste Time at the range's start puts it back.
	f.call("history.undo");
	f.call("range.cut", f.range({{"time", true}}));
	CHECK(f.spans(f.midiTrack) == Spans{{2 * g_second, g_start}, {g_start, 3 * g_second}});
	f.call("range.paste", {{"position", g_start}, {"time", true}});
	CHECK(f.spans(f.audioTrack) == Spans{{g_second, g_start}, {g_start, 3 * g_second}});
	CHECK(f.spans(f.midiTrack) == Spans{{2 * g_second, g_start}, {g_start, g_end}, {g_end, 4 * g_second}});

	// Onto other tracks: the first copied track goes to track, the next to the track below it. The types must fit.
	const int64_t audio2 = f.track("audio", "linear");
	const int64_t midi2 = f.track("instrument");
	f.call("range.paste", {{"position", 0}, {"track", audio2}});
	CHECK(f.spans(audio2) == Spans{{0, g_second / 2}});
	CHECK(f.spans(midi2) == Spans{{0, g_second}});
	CHECK_THAT(f.fails("range.paste", {{"track", f.midiTrack}}), ContainsSubstring("audio track"));
	CHECK_THAT(f.fails("range.paste", {{"track", midi2}}), ContainsSubstring("enough tracks"));
}

TEST_CASE("range.copy without a range copies everything between the locators: global copy", "[unit]")
{
	RangeFixture f;
	f.call("transport.set", {{"left", 2 * g_second}, {"right", 3 * g_second}});
	CHECK(f.call("range.copy").at("events") == 2);
	f.call("range.paste", {{"position", 6 * g_second}});
	CHECK(f.spans(f.audioTrack) == Spans{{g_second, 3 * g_second}, {6 * g_second, 7 * g_second}});
	CHECK(f.spans(f.midiTrack) == Spans{{2 * g_second, 4 * g_second}, {6 * g_second, 7 * g_second}});

	// A copied audio event whose clip is gone can't be pasted.
	f.call("range.cut");
	Json events = Json::array();
	const Json project = f.call("project.get");
	for (const Json& event : project.at("audioEvents"))
		events.push_back(event.at("id"));
	f.call("event.remove", {{"events", events}});
	f.call("pool.removeUnused");
	CHECK_THAT(f.fails("range.paste", {{"position", 0}}), ContainsSubstring("clip"));
}

TEST_CASE("range.delete and range.copy take the piece in the range of a clip at another sample rate", "[unit]")
{
	// A 44.1 kHz clip in a 48 kHz project: the piece that the cut at the range's end leaves inside it is 22056 frames
	// long, which ends a sample after the range.
	Fixture f;
	const int64_t track = f.track("audio", "linear");
	const Json media =
		f.call("media.add", {{"path", "slow.wav"}, {"sampleRate", 44100}, {"channels", 1}, {"length", 3 * 44100}})
			.at("media");
	const Json clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip");
	f.call("audioEvent.add", {{"track", track}, {"clip", clip.at("id")}, {"position", 0}, {"length", 3 * 44100}});
	const int64_t end = g_second + 24006;
	const Json range = {{"start", g_second}, {"end", end}, {"tracks", {track}}};

	CHECK(f.call("range.copy", range).at("events") == 1);
	f.call("range.delete", range);
	const Json project = f.call("project.get");
	REQUIRE(project.at("audioEvents").size() == 2);
	for (const Json& event : project.at("audioEvents"))
		CHECK((event.at("position") == 0 || event.at("position") == end));
}

TEST_CASE("Range edits take an event that starts a sample before an edge as after it", "[unit]")
{
	// A 22.05 kHz clip in a 48 kHz project: one project sample is less than half a clip frame, so a cut at the
	// range's end doesn't split the event, and only its first sample lies in the range.
	Fixture f;
	const int64_t track = f.track("audio", "linear");
	const Json media =
		f.call("media.add", {{"path", "slower.wav"}, {"sampleRate", 22050}, {"channels", 1}, {"length", 3 * 22050}})
			.at("media");
	const Json clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip");
	const int64_t end = g_second + 24006;
	f.call("audioEvent.add", {{"track", track}, {"clip", clip.at("id")}, {"position", end - 1}, {"length", 2 * 22050}});
	const Json before = f.call("project.get").at("audioEvents");
	const Json range = {{"start", g_second}, {"end", end}, {"tracks", {track}}};

	CHECK(f.call("range.copy", range).at("events") == 0);
	f.call("range.delete", range);
	CHECK(f.call("project.get").at("audioEvents") == before);

	// It comes after the range: Delete Time moves it back by the range's length, and Insert Silence at the range's
	// end moves it on by the silence's.
	const auto position = [&f] { return f.call("project.get").at("audioEvents").at(0).at("position"); };
	f.call("range.delete", {{"start", g_second}, {"end", end}, {"tracks", {track}}, {"time", true}});
	CHECK(position() == g_second - 1);
	f.call("history.undo");
	CHECK(f.call("project.get").at("audioEvents") == before);
	f.call("range.insertSilence", {{"start", end}, {"end", end + g_second}, {"tracks", {track}}});
	CHECK(position() == end - 1 + g_second);
}

TEST_CASE("range.paste doesn't paste what was copied in another project", "[unit]")
{
	RangeFixture f;
	f.call("range.copy", f.range());
	// The new project's tracks and clips get the old ones' ids again.
	f.call("project.new");
	const int64_t audioTrack = f.track("audio", "linear");
	f.track("midi");
	f.audioEvent(audioTrack);
	CHECK_THAT(f.fails("range.paste", {{"position", 0}}), ContainsSubstring("nothing to paste"));
}
