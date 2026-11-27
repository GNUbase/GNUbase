// SPDX-License-Identifier: GPL-3.0-or-later
// Audio parts (#471, reference manual p. 250): audioPart.add, event.toPart and event.dissolvePart, the event.*
// commands on audio parts, playback within a part and the project file. tests/eventediting_tests.cpp draws them in
// the event display.
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <variant>
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
using gnubase::time::g_ticksPerQuarter;

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

		// The ids of a part's audio events, by position.
		std::vector<Json> ids(const Json& part)
		{
			std::vector<Json> ids;
			for (const Json& event : eventsIn(part))
				ids.push_back(event.at("id"));
			return ids;
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

TEST_CASE("shared copies of an audio part share their events", "[unit]")
{
	Fixture f;
	const int64_t audio = f.track("audio");
	const int64_t other = f.track("audio");
	const int64_t first = f.audioEvent(audio, g_second, g_second);
	const Json part = f.call("event.toPart", {{"events", {first}}}).at("events").at(0);

	// Shared copies (reference manual p. 272) are one group, which event.get reports too.
	const Json copy =
		f.call("event.copy", {{"events", {part}}, {"delta", 4 * g_second}, {"track", other}, {"shared", true}})
			.at("events")
			.at(0);
	const Json twin = f.call("event.duplicate", {{"events", {copy}}, {"shared", true}}).at("events").at(0);
	const Json group = f.part(part).at("shared");
	CHECK(group != 0);
	CHECK(f.part(copy).at("shared") == group);
	CHECK(f.part(twin).at("shared") == group);
	CHECK(f.call("event.get", {{"events", {copy}}}).at("events").at(0).at("shared") == group);
	CHECK(f.part(f.call("event.copy", {{"events", {part}}}).at("events").at(0)).at("shared") == 0);
	f.call("history.undo");

	// An edit of one copy's events reaches the others, each at its own place, as one undo step.
	const Json event = f.eventsIn(copy).at(0).at("id");
	f.call("audioEvent.set", {{"event", event}, {"volume", -6.0}, {"position", (5 * g_second) + (g_second / 2)}});
	for (const auto& [each, start] : {std::pair{part, g_second}, std::pair{twin, 6 * g_second}})
	{
		const std::vector<Json> events = f.eventsIn(each);
		REQUIRE(events.size() == 1);
		CHECK(events[0].at("volume") == -6.0);
		CHECK(events[0].at("position") == start + (g_second / 2));
		CHECK(events[0].at("track") == f.part(each).at("track"));
	}
	f.call("history.undo");
	CHECK(f.eventsIn(part).at(0).at("volume") == 0.0);
	CHECK(f.eventsIn(twin).at(0).at("position") == 6 * g_second);
	CHECK(f.audio(event).at("volume") == 0.0);

	// A copy whose events move against its start leaves the group, so does one converted to a real copy, whose
	// events get clips of their own, and the last copy is a part like any other.
	f.call("event.resize", {{"events", {twin}}, {"edge", "start"}, {"delta", g_second / 4}});
	CHECK(f.part(twin).at("shared") == 0);
	CHECK(f.part(part).at("shared") == group);
	f.call("event.toRealCopy", {{"events", {copy}}});
	CHECK(f.part(copy).at("shared") == 0);
	CHECK(f.part(part).at("shared") == 0);
	CHECK(f.eventsIn(copy).at(0).at("clip") != f.eventsIn(part).at(0).at("clip"));
	f.call("audioEvent.set", {{"event", first}, {"volume", -3.0}});
	CHECK(f.eventsIn(copy).at(0).at("volume") == 0.0);

	// Splitting a shared copy takes both pieces out of the group.
	const Json pair = f.call("event.duplicate", {{"events", {part}}, {"shared", true}}).at("events").at(0);
	f.call("event.split", {{"events", {pair}}, {"positions", {(2 * g_second) + (g_second / 2)}}});
	CHECK(f.part(pair).at("shared") == 0);
	CHECK(f.part(part).at("shared") == 0);
}

TEST_CASE("shared copies of an audio part keep their events' ids, places in the stack and outside crossfades", "[unit]")
{
	Fixture f;
	const int64_t audio = f.track("audio");
	const int64_t first = f.audioEvent(audio, g_second, g_second);
	const int64_t second = f.audioEvent(audio, 2 * g_second, g_second, g_second);
	const Json part = f.call("event.toPart", {{"events", {first, second}}}).at("events").at(0);
	const Json copy =
		f.call("event.copy", {{"events", {part}}, {"delta", 10 * g_second}, {"shared", true}}).at("events").at(0);
	const std::vector<Json> copied = f.eventsIn(copy);
	REQUIRE(copied.size() == 2);
	const Json copyFirst = copied[0].at("id");
	const Json copySecond = copied[1].at("id");

	// Events outside the copy, added after it, in front of it: one the copy's first event crossfades from, one that
	// crossfades from its second.
	const int64_t before = f.audioEvent(audio, (10 * g_second) + (g_second / 2), g_second);
	const int64_t after = f.audioEvent(audio, (12 * g_second) + (g_second / 2), g_second);
	REQUIRE(f.audio(before).at("part") == 0);
	f.call("event.crossfade", {{"events", {copyFirst}}});
	f.call("event.crossfade", {{"events", {after}}});
	REQUIRE(f.audio(copyFirst).at("crossfade").at("from") == before);
	REQUIRE(f.audio(after).at("crossfade").at("from") == copySecond);
	CHECK(f.eventsIn(part).at(0).at("crossfade").is_null());

	// A transaction that edits an event in each copy keeps both edits in both copies, as one undo step: each copy's
	// events keep their ids.
	const auto volume = [](const Json& event, double db)
	{ return Json{{"command", "audioEvent.set"}, {"parameters", {{"event", event}, {"volume", db}}}}; };
	f.call("history.transaction",
		{{"description", "Volume"}, {"commands", {volume(first, -6.0), volume(copySecond, -3.0)}}});
	for (const Json& each : {part, copy})
	{
		const std::vector<Json> events = f.eventsIn(each);
		REQUIRE(events.size() == 2);
		CHECK(events[0].at("volume") == -6.0);
		CHECK(events[1].at("volume") == -3.0);
	}
	CHECK(f.eventsIn(copy).at(0).at("id") == copyFirst);
	CHECK(f.eventsIn(copy).at(1).at("id") == copySecond);

	// The outside events stay in front of the copy's, and both crossfades play on.
	const auto played = [&f, audio](const Json& id)
	{
		const RenderChannel& channel = f.channel(audio);
		const auto found = std::find_if(channel.audioEvents.begin(), channel.audioEvents.end(),
			[&id](const gnubase::engine::RenderAudioEvent& event) { return event.id == id.get<ObjectId>(); });
		REQUIRE(found != channel.audioEvents.end());
		return *found;
	};
	CHECK(played(before).front > played(copyFirst).front);
	CHECK(played(after).front > played(copySecond).front);
	CHECK(played(copyFirst).crossfade.from == static_cast<ObjectId>(before));
	CHECK(played(after).crossfade.from == copySecond.get<ObjectId>());
	CHECK(f.audio(copyFirst).at("crossfade").at("from") == before);

	f.call("history.undo");
	for (const Json& each : {part, copy})
		for (const Json& event : f.eventsIn(each))
			CHECK(event.at("volume") == 0.0);

	// Bringing one copy to the front leaves the other where it is in the stack.
	const ObjectId place = played(copySecond).front;
	f.call("event.toFront", {{"events", {part}}});
	CHECK(played(second).front > place);
	CHECK(played(copySecond).front == place);
	CHECK(played(after).front > played(copySecond).front);
}

TEST_CASE("shared copies of an audio part on tracks of different time bases pair their events by place", "[unit]")
{
	// A musical original with one clip's hit used twice, at 1 and 5 beats from the part's start, and a shared copy on a
	// linear track: there, at 120 BPM, the places are 24000 and 120000 samples, whose digits sort the other way.
	Fixture f;
	const int64_t musical =
		f.call("track.add", {{"type", "audio"}, {"timeBase", "musical"}}).at("track").at("id").get<int64_t>();
	const int64_t linear = f.track("audio");
	const int64_t start = f.audioEvent(musical, 0, g_second / 4);
	const int64_t one = f.audioEvent(musical, g_ticksPerQuarter, g_second / 2);
	const int64_t five = f.audioEvent(musical, 5 * g_ticksPerQuarter, g_second / 2);
	const Json part = f.call("event.toPart", {{"events", {start, one, five}}}).at("events").at(0);
	const Json copy =
		f.call("event.copy", {{"events", {part}}, {"delta", 10 * g_second}, {"track", linear}, {"shared", true}})
			.at("events")
			.at(0);
	std::vector<Json> ids;
	for (const Json& event : f.eventsIn(copy))
		ids.push_back(event.at("id"));
	REQUIRE(ids.size() == 3);

	// An edit in the original leaves each of the copy's events with its id at its place.
	f.call("audioEvent.set", {{"event", one}, {"volume", -6.0}});
	const std::vector<Json> events = f.eventsIn(copy);
	REQUIRE(events.size() == 3);
	for (size_t i = 0; i < events.size(); ++i)
		CHECK(events[i].at("id") == ids[i]);
	CHECK(events[1].at("volume") == -6.0);
	CHECK(events[2].at("volume") == 0.0);
}

TEST_CASE(
	"shared copies of an audio part pair their events without the slice information only the original keeps", "[unit]")
{
	// A loop in a part sliced at 2 seconds into its clip, and its second slice copied to an earlier place in the part.
	// The copy has the slice's clip section but, as the user's own, not what the Reset of the Slice Rules needs, and
	// the part's shared copy has none of that at all.
	Fixture f;
	const int64_t audio = f.track("audio");
	const int64_t loop = f.audioEvent(audio, g_second, 3 * g_second);
	const Json part = f.call("event.toPart", {{"events", {loop}}}).at("events").at(0);
	f.call("clip.setHitpoints",
		{{"clip", f.clip}, {"hitpoints", {{{"position", 2 * g_second}, {"peak", -3.0}, {"intensity", 1.0}}}}});
	f.call("quantize.slice", {{"events", {loop}}});
	const std::vector<Json> slices = f.eventsIn(part);
	REQUIRE(slices.size() == 2);
	REQUIRE(slices[1].at("slicedFrom") == loop);
	f.call("event.copy", {{"events", {slices[1].at("id")}}, {"delta", -(3 * g_second) / 2}});
	REQUIRE(f.eventsIn(part).size() == 3);
	const Json copy =
		f.call("event.copy", {{"events", {part}}, {"delta", 10 * g_second}, {"shared", true}}).at("events").at(0);
	const std::vector<Json> ids = f.ids(copy);
	REQUIRE(ids.size() == 3);

	// An edit in the original leaves each of the copy's events with its id at its place.
	f.call("audioEvent.set", {{"event", loop}, {"volume", -6.0}});
	CHECK(f.ids(copy) == ids);
	CHECK(f.eventsIn(copy).at(0).at("volume") == -6.0);
}

TEST_CASE("shared copies of an audio part pair their events without the quantize origin a save keeps in one time base",
	"[integration]")
{
	// A musical original with one clip section used twice, the later one quantized from a 32nd note late onto beat 3,
	// and a shared copy on a linear track. A save keeps the quantize origin only in the event's own time base, so the
	// copy's is gone after a load.
	const gnubase::test::TempDir dir;
	Fixture f;
	const int64_t musical =
		f.call("track.add", {{"type", "audio"}, {"timeBase", "musical"}}).at("track").at("id").get<int64_t>();
	const int64_t linear = f.track("audio");
	const int64_t early = f.audioEvent(musical, g_ticksPerQuarter, g_second / 4);
	const int64_t late = f.audioEvent(musical, (3 * g_ticksPerQuarter) + (g_ticksPerQuarter / 8), g_second / 4);
	const Json part = f.call("event.toPart", {{"events", {early, late}}}).at("events").at(0);
	f.call("quantize.set", {{"preset", "1/4"}});
	f.call("quantize.apply", {{"events", {late}}});
	REQUIRE(f.audio(late).at("position") == 3 * g_ticksPerQuarter);
	REQUIRE(f.audio(late).contains("quantizeOrigin"));
	const Json copy =
		f.call("event.copy", {{"events", {part}}, {"delta", 10 * g_second}, {"track", linear}, {"shared", true}})
			.at("events")
			.at(0);
	const std::string file = (dir.path() / "quantized.dawproject").u8string();
	f.call("project.save", {{"path", file}, {"embedMedia", false}});
	f.call("project.new");
	f.call("project.load", {{"path", file}});
	REQUIRE(f.audio(late).contains("quantizeOrigin"));
	const std::vector<Json> ids = f.ids(copy);
	REQUIRE(ids.size() == 2);
	REQUIRE_FALSE(f.audio(ids[1]).contains("quantizeOrigin"));

	// An edit in the original leaves each of the copy's events with its id at its place.
	f.call("audioEvent.set", {{"event", early}, {"volume", -6.0}});
	CHECK(f.ids(copy) == ids);
	CHECK(f.eventsIn(copy).at(0).at("volume") == -6.0);
}

TEST_CASE("event.fillLoop and track.duplicate keep shared copies of audio parts in their group", "[unit]")
{
	Fixture f;
	const int64_t audio = f.track("audio");
	const Json part =
		f.call("event.toPart", {{"events", {f.audioEvent(audio, g_second, g_second)}}}).at("events").at(0);

	// Copies at 2 and 3 seconds, the last cut at 3.5 seconds, still a shared copy.
	const Json copies =
		f.call("event.fillLoop",
			 {{"events", {part}}, {"start", g_second}, {"end", (3 * g_second) + (g_second / 2)}, {"shared", true}})
			.at("events");
	REQUIRE(copies.size() == 2);
	const Json group = f.part(part).at("shared");
	CHECK(group != 0);
	CHECK(f.part(copies[0]).at("shared") == group);
	CHECK(f.part(copies[1]).at("shared") == group);
	CHECK(f.part(copies[1]).at("length") == g_second / 2);
	const Json event = f.eventsIn(part).at(0).at("id");
	f.call("audioEvent.set", {{"event", event}, {"volume", -6.0}});
	CHECK(f.eventsIn(copies[1]).at(0).at("volume") == -6.0);

	// A duplicated track's parts are copies in the same group, as MIDI parts are.
	const Json track = f.call("track.duplicate", {{"track", audio}}).at("track").at("id");
	std::vector<Json> duplicates;
	const Json project = f.call("project.get");
	for (const Json& each : project.at("audioParts"))
		if (each.at("track") == track)
			duplicates.push_back(each.at("id"));
	REQUIRE(duplicates.size() == 3);
	for (const Json& each : duplicates)
		CHECK(f.part(each).at("shared") == group);
	f.call("audioEvent.set", {{"event", event}, {"volume", -3.0}});
	for (const Json& each : duplicates)
		CHECK(f.eventsIn(each).at(0).at("volume") == -3.0);
}

TEST_CASE("gluing, sizing the end with the contents, a range paste, a track import and a stashed version make real "
		  "copies of shared audio parts",
	"[integration]")
{
	Fixture f;
	const int64_t audio = f.track("audio");
	const int64_t first = f.audioEvent(audio, g_second, g_second);
	const Json part = f.call("event.toPart", {{"events", {first}}}).at("events").at(0);
	const auto group = [&f](const Json& id) { return f.part(id).at("shared"); };
	const auto sharedCopy = [&f, &part](const Json& parameters)
	{
		Json copy = parameters;
		copy["events"] = Json::array({part});
		copy["shared"] = true;
		return f.call("event.copy", copy).at("events").at(0);
	};
	// The parts on a track other than those named.
	const auto others = [&f](int64_t track, const std::vector<Json>& named)
	{
		std::vector<Json> result;
		const Json project = f.call("project.get");
		for (const Json& each : project.at("audioParts"))
			if (each.at("track") == track && std::find(named.begin(), named.end(), each.at("id")) == named.end())
				result.push_back(each.at("id"));
		return result;
	};

	SECTION("gluing")
	{
		const Json copy = sharedCopy({{"delta", 4 * g_second}});
		const int64_t next = f.audioEvent(audio, 6 * g_second, g_second);
		f.call("event.glue", {{"events", {copy, next}}});
		CHECK(group(copy) == 0);
		CHECK(group(part) == 0);
	}
	SECTION("sizing the end with the contents")
	{
		const Json copy = sharedCopy({{"delta", 4 * g_second}});
		f.call(
			"event.resize", {{"events", {copy}}, {"edge", "end"}, {"delta", g_second / 2}, {"mode", "moveContents"}});
		CHECK(group(copy) == 0);
		CHECK(group(part) == 0);
	}
	SECTION("a range paste")
	{
		const Json copy = sharedCopy({{"delta", 4 * g_second}});
		f.call("range.copy", {{"start", 0}, {"end", 3 * g_second}, {"tracks", {audio}}});
		f.call("range.paste", {{"position", 8 * g_second}});
		const std::vector<Json> pasted = others(audio, {part, copy});
		REQUIRE(pasted.size() == 1);
		CHECK(group(pasted[0]) == 0);
		CHECK(group(part) != 0);
		CHECK(group(copy) == group(part));
	}
	SECTION("a track import")
	{
		const Json copy = sharedCopy({{"delta", 4 * g_second}});
		const gnubase::test::TempDir dir;
		const std::string file = (dir.path() / "track.dawproject").u8string();
		f.call("track.export", {{"tracks", {audio}}, {"path", file}, {"copyMedia", false}});
		const int64_t imported = f.call("track.import", {{"path", file}}).at("tracks").at(0).get<int64_t>();
		const std::vector<Json> parts = others(imported, {});
		REQUIRE(parts.size() == 2);
		CHECK(group(parts[0]) == 0);
		CHECK(group(parts[1]) == 0);
		CHECK(group(copy) == group(part));

		// So do the shared copies in a version that doesn't play.
		f.call("trackVersion.duplicate", {{"tracks", {audio}}});
		f.call("track.export", {{"tracks", {audio}}, {"path", file}, {"copyMedia", false}});
		const int64_t again = f.call("track.import", {{"path", file}}).at("tracks").at(0).get<int64_t>();
		Json versions;
		Json active;
		const Json project = f.call("project.get");
		for (const Json& each : project.at("tracks"))
			if (each.at("id") == again)
			{
				versions = each.at("versions");
				active = each.at("activeVersion");
			}
		REQUIRE(versions.size() == 2);
		const Json stashed = versions[0].at("id") == active ? versions[1].at("id") : versions[0].at("id");
		f.call("trackVersion.activate", {{"tracks", {again}}, {"version", stashed}});
		const std::vector<Json> copies = others(again, {});
		REQUIRE(copies.size() == 2);
		CHECK(group(copies[0]) == 0);
		CHECK(group(copies[1]) == 0);
	}
	SECTION("a stashed version")
	{
		const int64_t other = f.track("audio");
		const Json copy = sharedCopy({{"track", other}});
		f.call("trackVersion.add", {{"tracks", {audio}}});
		CHECK(group(copy) == 0); // the last of its copies
		f.call("trackVersion.activate", {{"tracks", {audio}}, {"version", 1}});
		CHECK(group(part) == 0);
		f.call("audioEvent.set", {{"event", f.eventsIn(part).at(0).at("id")}, {"volume", -6.0}});
		CHECK(f.eventsIn(copy).at(0).at("volume") == 0.0);
	}
}

TEST_CASE(
	"a version keeps the shared audio parts that all go into it, and its duplicate gets a group of its own", "[unit]")
{
	Fixture f;
	const int64_t audio = f.track("audio");
	const Json part =
		f.call("event.toPart", {{"events", {f.audioEvent(audio, g_second, g_second)}}}).at("events").at(0);
	const Json copy =
		f.call("event.copy", {{"events", {part}}, {"delta", 4 * g_second}, {"shared", true}}).at("events").at(0);
	const Json group = f.part(part).at("shared");
	REQUIRE(group != 0);

	// Both copies go into the stashed version and come back as one group, edited together as MIDI parts are.
	f.call("trackVersion.add", {{"tracks", {audio}}});
	f.call("trackVersion.activate", {{"tracks", {audio}}, {"version", 1}});
	CHECK(f.part(part).at("shared") == group);
	CHECK(f.part(copy).at("shared") == group);
	f.call("audioEvent.set", {{"event", f.eventsIn(part).at(0).at("id")}, {"volume", -6.0}});
	CHECK(f.eventsIn(copy).at(0).at("volume") == -6.0);

	// A duplicate version's copies are a group of their own, apart from the original's.
	f.call("trackVersion.duplicate", {{"tracks", {audio}}});
	std::vector<Json> duplicates;
	const Json project = f.call("project.get");
	for (const Json& each : project.at("audioParts"))
		if (each.at("track") == audio)
			duplicates.push_back(each.at("id"));
	REQUIRE(duplicates.size() == 2);
	const Json own = f.part(duplicates[0]).at("shared");
	CHECK(own != 0);
	CHECK(own != group);
	CHECK(f.part(duplicates[1]).at("shared") == own);
	f.call("audioEvent.set", {{"event", f.eventsIn(duplicates[0]).at(0).at("id")}, {"volume", -3.0}});
	CHECK(f.eventsIn(duplicates[1]).at(0).at("volume") == -3.0);
	f.call("trackVersion.activate", {{"tracks", {audio}}, {"version", 1}});
	CHECK(f.eventsIn(copy).at(0).at("volume") == -6.0);
}

TEST_CASE("a dissolved muted part leaves its events muted, as they played", "[unit]")
{
	Fixture f;
	const int64_t audio = f.track("audio");
	const int64_t first = f.audioEvent(audio, g_second, g_second);
	const int64_t second = f.audioEvent(audio, 3 * g_second, g_second);
	const Json part = f.call("event.toPart", {{"events", {first, second}}}).at("events").at(0);
	f.call("event.set", {{"events", {part}}, {"mute", true}});
	f.call("event.dissolvePart", {{"events", {part}}});
	CHECK(f.audio(first).at("mute") == true);
	CHECK(f.audio(second).at("mute") == true);
	CHECK(f.channel(audio).audioEvents.size() == 2);
	CHECK(f.channel(audio).audioEvents[0].muted);
}

TEST_CASE("splitting an audio part announces the events moved into the new part as changed", "[unit]")
{
	Fixture f;
	const int64_t audio = f.track("audio");
	const int64_t across = f.audioEvent(audio, g_second, 2 * g_second);
	const int64_t after = f.audioEvent(audio, 4 * g_second, g_second);
	const Json part = f.call("event.toPart", {{"events", {across, after}}}).at("events").at(0);
	while (f.client.poll())
	{
	}
	const Json right = f.call("event.split", {{"events", {part}}, {"positions", {2 * g_second}}}).at("events").at(0);
	const std::vector<Json> moved = f.eventsIn(right);
	REQUIRE(moved.size() == 2);
	const int64_t piece = moved[0].at("id").get<int64_t>();
	std::map<std::string, std::vector<int64_t>> announced;
	while (const auto message = f.client.poll())
		if (const auto* event = std::get_if<gnubase::engine::Event>(&*message);
			event != nullptr && event->name.rfind("audioEvent.", 0) == 0)
			announced[event->name].push_back(event->data.at("event").at("id").get<int64_t>());
	CHECK(announced["audioEvent.added"] == std::vector<int64_t>{piece});
	CHECK(std::count(announced["audioEvent.changed"].begin(), announced["audioEvent.changed"].end(), after) >= 1);
	CHECK(std::count(announced["audioEvent.changed"].begin(), announced["audioEvent.changed"].end(), across) >= 1);
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
	f.call("event.set", {{"events", {part}}, {"mute", true}, {"color", "#123456"}});
	f.call("audioPart.add", {{"track", audio}, {"position", 6 * g_second}, {"length", g_second}, {"name", "Empty"}});
	f.call("event.copy", {{"events", {part}}, {"delta", 8 * g_second}, {"shared", true}});
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
	REQUIRE(loaded.at("audioParts").size() == 3); // a shared copy as an ordinary clip
	const Json& muted = loaded.at("audioParts").at(0);
	CHECK(muted.at("mute") == true);
	CHECK(f.call("event.get", {{"events", {muted.at("id")}}}).at("events").at(0).at("start") == g_second);
	CHECK(f.call("event.get", {{"events", {muted.at("id")}}}).at("events").at(0).at("end") == 4 * g_second);
	CHECK(loaded.at("audioParts").at(1).at("name") == "Empty");
	const std::vector<Json> events = f.eventsIn(muted.at("id"));
	REQUIRE(events.size() == 2);
	CHECK(events[0].at("mute") == false);
	CHECK(muted.at("color") == "#123456");
	CHECK(events[0].at("color").is_null()); // the part's color is the part's only
	CHECK(f.call("event.get", {{"events", {events[1].at("id")}}}).at("events").at(0).at("start") == 3 * g_second);
}
