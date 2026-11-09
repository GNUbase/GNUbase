// SPDX-License-Identifier: GPL-3.0-or-later
// Audio parts (#471, reference manual p. 250): audioPart.add, event.toPart and event.dissolvePart, the event.*
// commands on audio parts, playback within a part and the project file. tests/eventediting_tests.cpp draws them in
// the event display.
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using gnubase::engine::Client;
using gnubase::engine::Engine;
using gnubase::engine::Json;
using gnubase::engine::ObjectId;
using gnubase::engine::RenderChannel;
using gnubase::engine::RenderSnapshot;
using gnubase::test::readZip;
using gnubase::test::writeZip;

namespace
{
	constexpr int64_t g_second = 48000;

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
			REQUIRE(!result.ok());
			return result.error;
		}

		int64_t track(const char* type)
		{
			return call("track.add", {{"type", type}, {"timeBase", "linear"}}).at("track").at("id").get<int64_t>();
		}

		// An audio event at position (project samples) playing length samples of a 10 second clip from offset.
		int64_t audioEvent(int64_t onTrack, int64_t position, int64_t length, int64_t offset = 0)
		{
			if (clip == 0)
			{
				const Json media = call("media.add",
					{{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 10 * g_second}})
									   .at("media");
				clip = call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
			}
			return call("audioEvent.add",
				{{"track", onTrack}, {"clip", clip}, {"position", position}, {"offset", offset}, {"length", length}})
				.at("event")
				.at("id")
				.get<int64_t>();
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
		Json part(const Json& id) { return object("audioParts", id); }
		size_t count(const char* kind) { return call("project.get").at(kind).size(); }

		// The audio events of a part, by position.
		std::vector<Json> eventsIn(const Json& part)
		{
			std::vector<Json> events;
			const Json project = call("project.get");
			for (const Json& event : project.at("audioEvents"))
				if (event.at("part") == part)
					events.push_back(event);
			std::sort(events.begin(), events.end(),
				[](const Json& a, const Json& b) { return a.at("position") < b.at("position"); });
			return events;
		}

		const RenderChannel& channel(int64_t onTrack)
		{
			const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
			const auto found = std::find_if(snapshot.channels.begin(), snapshot.channels.end(),
				[onTrack](const RenderChannel& each) { return each.track == static_cast<ObjectId>(onTrack); });
			REQUIRE(found != snapshot.channels.end());
			return *found;
		}

		Engine engine;
		Client client{engine};
		int64_t clip = 0;
	};
}

TEST_CASE("audioPart.add adds an empty audio part to an audio track", "[unit]")
{
	Fixture f;
	const int64_t audio = f.track("audio");
	const Json part =
		f.call("audioPart.add", {{"track", audio}, {"position", g_second}, {"length", 2 * g_second}}).at("part");
	CHECK(part.at("track") == audio);
	CHECK(part.at("position") == g_second);
	CHECK(part.at("length") == 2 * g_second);
	CHECK(part.at("name") == f.call("project.get").at("tracks").back().at("name"));
	CHECK(part.at("mute") == false);
	const Json info = f.call("event.get", {{"events", {part.at("id")}}}).at("events").at(0);
	CHECK(info.at("kind") == "audioPart");
	CHECK(info.at("start") == g_second);
	CHECK(info.at("end") == 3 * g_second);

	CHECK_THAT(f.fails("audioPart.add", {{"track", f.track("midi")}, {"position", 0}, {"length", 1}}),
		ContainsSubstring("audio track"));
	CHECK_THAT(
		f.fails("audioPart.add", {{"track", audio}, {"position", 0}, {"length", 0}}), ContainsSubstring("positive"));
	f.call("history.undo"); // the MIDI track
	CHECK(f.count("audioParts") == 1);
	f.call("history.undo");
	CHECK(f.count("audioParts") == 0);
}

TEST_CASE("Events to Part puts audio events into a part, Dissolve Part takes them out again", "[unit]")
{
	Fixture f;
	const int64_t audio = f.track("audio");
	const int64_t other = f.track("audio");
	const int64_t first = f.audioEvent(audio, g_second, g_second);
	const int64_t second = f.audioEvent(audio, 3 * g_second, g_second, 2 * g_second);
	const int64_t elsewhere = f.audioEvent(other, 0, g_second);

	// One part per track, from the first start to the last end.
	const Json parts = f.call("event.toPart", {{"events", {second, first, elsewhere}}}).at("events");
	REQUIRE(parts.size() == 2);
	const Json part = f.part(parts[0]);
	CHECK(part.at("track") == audio);
	CHECK(part.at("position") == g_second);
	CHECK(part.at("length") == 3 * g_second);
	CHECK(f.audio(first).at("part") == parts[0]);
	CHECK(f.audio(second).at("part") == parts[0]);
	CHECK(f.audio(second).at("position") == 3 * g_second); // events keep their place
	CHECK(f.audio(elsewhere).at("part") == parts[1]);
	CHECK_THAT(f.fails("event.toPart", {{"events", {first}}}), ContainsSubstring("outside a part"));
	CHECK_THAT(f.fails("event.toPart", {{"events", {parts[0]}}}), ContainsSubstring("outside a part"));

	// One undo step.
	f.call("history.undo");
	CHECK(f.count("audioParts") == 0);
	CHECK(f.audio(first).at("part") == 0);
	f.call("history.redo");
	CHECK(f.count("audioParts") == 2);

	// Dissolving keeps what the part plays: an event sticking out of it is cut at its edge, one it hides goes.
	f.call("event.resize", {{"events", {parts[0]}}, {"edge", "end"}, {"delta", -(g_second / 2)}});
	const int64_t hidden = f.audioEvent(audio, 8 * g_second, g_second);
	f.call("event.toPart", {{"events", {hidden}}}); // a part of its own, then moved into the first one
	REQUIRE(f.audio(hidden).at("part") != parts[0]);
	const Json glued = f.call("event.glue", {{"events", {parts[0], f.audio(hidden).at("part")}}});
	CHECK(glued.at("event") == parts[0]);
	f.call("event.resize", {{"events", {parts[0]}}, {"edge", "end"}, {"delta", -((5 * g_second) + (g_second / 2))}});
	CHECK(f.part(parts[0]).at("length") == (2 * g_second) + (g_second / 2));

	const Json events = f.call("event.dissolvePart", {{"events", {parts[0]}}}).at("events");
	CHECK(events.size() == 2);
	CHECK(f.count("audioParts") == 1);
	CHECK(f.count("audioEvents") == 3);
	CHECK(f.audio(first).at("part") == 0);
	CHECK(f.audio(first).at("length") == g_second);
	const Json cut = f.audio(events[1]);
	CHECK(cut.at("part") == 0);
	CHECK(cut.at("position") == 3 * g_second);
	CHECK(cut.at("length") == g_second / 2);
	CHECK_THAT(f.fails("event.dissolvePart", {{"events", {first}}}), ContainsSubstring("isn't an audio part"));
	f.call("history.undo");
	CHECK(f.count("audioParts") == 2);
	CHECK(f.count("audioEvents") == 4);
}

TEST_CASE("audio parts move, copy, split, glue, mute, lock and go with their events", "[unit]")
{
	Fixture f;
	const int64_t audio = f.track("audio");
	const int64_t other = f.track("audio");
	const int64_t first = f.audioEvent(audio, g_second, g_second);
	const int64_t second = f.audioEvent(audio, 2 * g_second, 2 * g_second, g_second);
	const Json part = f.call("event.toPart", {{"events", {first, second}}}).at("events").at(0);

	// Moving takes the events along, also onto another audio track.
	f.call("event.move", {{"events", {part}}, {"delta", g_second}, {"track", other}});
	CHECK(f.part(part).at("position") == 2 * g_second);
	CHECK(f.part(part).at("track") == other);
	CHECK(f.audio(first).at("position") == 2 * g_second);
	CHECK(f.audio(second).at("track") == other);
	CHECK_THAT(
		f.fails("event.move", {{"events", {part}}, {"track", f.track("midi")}}), ContainsSubstring("audio track"));
	CHECK_THAT(f.fails("event.move", {{"events", {first}}, {"track", audio}}), ContainsSubstring("part's track"));
	f.call("history.undo"); // the MIDI track

	// A copy has copies of the events.
	const Json copy = f.call("event.copy", {{"events", {part}}, {"delta", 4 * g_second}}).at("events").at(0);
	const std::vector<Json> copied = f.eventsIn(copy);
	REQUIRE(copied.size() == 2);
	CHECK(copied[0].at("position") == 6 * g_second);
	CHECK(copied[0].at("id") != first);
	CHECK(f.eventsIn(part).size() == 2);

	// Splitting cuts the event across the position; what lies after it goes into the new part.
	const int64_t splitAt = (7 * g_second) + (g_second / 2);
	const Json right = f.call("event.split", {{"events", {copy}}, {"positions", {splitAt}}}).at("events").at(0);
	CHECK(f.part(copy).at("length") == g_second + (g_second / 2));
	CHECK(f.part(right).at("position") == splitAt);
	CHECK(f.eventsIn(copy).size() == 2);
	const std::vector<Json> after = f.eventsIn(right);
	REQUIRE(after.size() == 1);
	CHECK(after[0].at("position") == splitAt);
	CHECK(after[0].at("length") == g_second + (g_second / 2));

	// Gluing the two pieces makes one part again with all three events.
	f.call("event.glue", {{"events", {right, copy}}});
	CHECK(f.part(copy).at("length") == 3 * g_second);
	CHECK(f.eventsIn(copy).size() == 3);
	CHECK(f.count("audioParts") == 2);

	// Audio events that don't continue each other glue into a new part (reference manual p. 269).
	const int64_t a = f.audioEvent(audio, 0, g_second);
	const int64_t b = f.audioEvent(audio, 2 * g_second, g_second);
	const Json made = f.call("event.glue", {{"events", {a, b}}}).at("event");
	CHECK(f.part(made).at("length") == 3 * g_second);
	CHECK(f.audio(b).at("part") == made);

	// Mute and lock.
	f.call("event.set", {{"events", {part}}, {"mute", true}, {"lock", true}, {"color", "#123456"}});
	CHECK(f.part(part).at("mute") == true);
	CHECK(f.part(part).at("color") == "#123456");
	CHECK_THAT(f.fails("event.remove", {{"events", {part}}}), ContainsSubstring("locked"));
	f.call("event.set", {{"events", {part}}, {"lock", false}});

	// Removing a part removes its events, as one undo step.
	const size_t events = f.count("audioEvents");
	f.call("event.remove", {{"events", {part}}});
	CHECK(f.count("audioEvents") == events - 2);
	f.call("history.undo");
	CHECK(f.count("audioEvents") == events);
	CHECK(f.audio(first).at("part") == part);

	// Duplicating copies the events too; removing the track removes its parts.
	f.call("event.duplicate", {{"events", {made}}});
	CHECK(f.count("audioParts") == 4);
	f.call("track.remove", {{"track", audio}});
	CHECK(f.count("audioParts") == 2);
}

TEST_CASE("an audio part's events play only within it, and not while it is muted", "[unit]")
{
	Fixture f;
	const int64_t audio = f.track("audio");
	const int64_t inside = f.audioEvent(audio, g_second, g_second, g_second);
	const int64_t across = f.audioEvent(audio, 3 * g_second, 2 * g_second);
	const int64_t outside = f.audioEvent(audio, 6 * g_second, g_second);
	const Json part = f.call("event.toPart", {{"events", {inside, across, outside}}}).at("events").at(0);
	f.call("event.resize", {{"events", {part}}, {"edge", "start"}, {"delta", g_second + (g_second / 2)}});
	f.call("event.resize", {{"events", {part}}, {"edge", "end"}, {"delta", -((2 * g_second) + (g_second / 2))}});
	REQUIRE(f.part(part).at("position") == (2 * g_second) + (g_second / 2));
	REQUIRE(f.part(part).at("length") == 2 * g_second);

	const RenderChannel& channel = f.channel(audio);
	REQUIRE(channel.audioEvents.size() == 1);
	CHECK(channel.audioEvents[0].id == static_cast<ObjectId>(across));
	CHECK(channel.audioEvents[0].start == 3 * g_second);
	CHECK(channel.audioEvents[0].offset == 0);
	CHECK(channel.audioEvents[0].length == 3 * (g_second / 2));

	f.call("event.resize", {{"events", {part}}, {"edge", "start"}, {"delta", -g_second}});
	const RenderChannel& more = f.channel(audio);
	REQUIRE(more.audioEvents.size() == 2);
	CHECK(more.audioEvents[0].start == 3 * (g_second / 2));
	CHECK(more.audioEvents[0].offset == g_second + (g_second / 2));
	CHECK(more.audioEvents[0].length == g_second / 2);

	f.call("event.set", {{"events", {part}}, {"mute", true}});
	CHECK(f.channel(audio).audioEvents.empty());
}

TEST_CASE("audio parts are saved in the DAWproject file", "[integration]")
{
	const gnubase::test::TempDir dir;
	Fixture f;
	const int64_t audio = f.track("audio");
	const int64_t first = f.audioEvent(audio, g_second, g_second);
	const int64_t second = f.audioEvent(audio, 3 * g_second, g_second, g_second);
	const Json part = f.call("event.toPart", {{"events", {first, second}}}).at("events").at(0);
	f.call("event.set", {{"events", {part}}, {"mute", true}});
	f.call("audioPart.add", {{"track", audio}, {"position", 6 * g_second}, {"length", g_second}, {"name", "Empty"}});
	const Json before = f.call("project.get");
	const std::string file = (dir.path() / "parts.dawproject").u8string();
	f.call("project.save", {{"path", file}, {"embedMedia", false}});

	// GNUbase's entry brings them back exactly.
	f.call("project.new");
	f.call("project.load", {{"path", file}});
	CHECK(f.call("project.get").at("audioParts") == before.at("audioParts"));
	CHECK(f.call("project.get").at("audioEvents") == before.at("audioEvents"));

	// project.xml has each part as a clip of its events' clips.
	std::map<std::string, std::string> entries = readZip(file);
	REQUIRE(entries.erase("gnubase.json") == 1);
	const std::string other = (dir.path() / "other.dawproject").u8string();
	writeZip(other, entries);
	f.call("project.load", {{"path", other}});
	const Json loaded = f.call("project.get");
	REQUIRE(loaded.at("audioParts").size() == 2);
	const Json& muted = loaded.at("audioParts").at(0);
	CHECK(muted.at("mute") == true);
	CHECK(f.call("event.get", {{"events", {muted.at("id")}}}).at("events").at(0).at("start") == g_second);
	CHECK(f.call("event.get", {{"events", {muted.at("id")}}}).at("events").at(0).at("end") == 4 * g_second);
	CHECK(loaded.at("audioParts").at(1).at("name") == "Empty");
	const std::vector<Json> events = f.eventsIn(muted.at("id"));
	REQUIRE(events.size() == 2);
	CHECK(events[0].at("mute") == false);
	CHECK(f.call("event.get", {{"events", {events[1].at("id")}}}).at("events").at(0).at("start") == 3 * g_second);
}
