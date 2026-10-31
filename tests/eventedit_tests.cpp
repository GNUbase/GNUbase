// SPDX-License-Identifier: GPL-3.0-or-later
// Editing parts and events (#85): the event.* commands on audio events and MIDI parts, shared copies and locks.
// tests/eventdisplay_tests.cpp drives the same commands through the event display's gestures.
#include "engine/engine.h"
#include "engine/time/tempomap.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <string>

using Catch::Matchers::ContainsSubstring;
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
	CHECK_THAT(f.fails("event.move", {{"events", {12345}}}), ContainsSubstring("no audio event or MIDI part"));

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

	// Audio events that don't continue each other don't glue.
	const Json copy = f.call("event.copy", {{"events", {event}}, {"delta", 3 * g_second}}).at("events");
	CHECK_THAT(f.fails("event.glue", {{"events", {event, copy[0]}}}), ContainsSubstring("continues"));

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
