// SPDX-License-Identifier: GPL-3.0-or-later
// Lanes and comping (#99, reference manual pp. 225-228): event.toFront brings takes to the front, event.comp brings a
// lane's takes to the front in a range, and what plays follows. docs/features/comping.md describes them.
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using gnubase::engine::Client;
using gnubase::engine::Engine;
using gnubase::engine::Json;

namespace
{
	constexpr int64_t g_quarter = gnubase::time::g_ticksPerQuarter;
	constexpr int64_t g_bar = 4 * g_quarter;
	constexpr int64_t g_barSamples = 96000; // at 120 BPM

	// Frame f of the ramp file is (f + 1) / 65536: exact in float, so each take is told apart sample by sample.
	float ramp(int64_t frame)
	{
		return static_cast<float>(frame + 1) / 65536.0f;
	}

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

		int64_t track(const char* type, const char* timeBase = "linear")
		{
			return call("track.add", {{"type", type}, {"timeBase", timeBase}}).at("track").at("id").get<int64_t>();
		}

		// A take of the ramp clip on a lane: from position 0, playing the clip from offset for length frames.
		int64_t take(int64_t onTrack, int64_t clip, int64_t offset, int64_t length, int lane)
		{
			const auto id = call("audioEvent.add",
				{{"track", onTrack}, {"clip", clip}, {"position", 0}, {"offset", offset}, {"length", length}})
								.at("event")
								.at("id")
								.get<int64_t>();
			call("event.set", {{"events", {id}}, {"lane", lane}});
			return id;
		}

		int64_t part(int64_t onTrack, int lane, bool mute)
		{
			const auto id = call("midiPart.add", {{"track", onTrack}, {"position", 0}, {"length", 2 * g_bar}})
								.at("part")
								.at("id")
								.get<int64_t>();
			call("event.set", {{"events", {id}}, {"lane", lane}, {"mute", mute}});
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

		// Plays from the project start and returns the left output channel's first frames.
		std::vector<float> render(int64_t frames)
		{
			call("transport.locate", {{"position", 0}});
			call("transport.play");
			call("audio.render", {{"frames", frames}});
			std::vector<float> output =
				call("audio.capture", {{"channel", 0}, {"start", m_rendered}, {"frames", frames}})
					.at("samples")
					.get<std::vector<float>>();
			call("transport.stop");
			call("audio.render", {{"frames", 256}});
			m_rendered += frames + 256;
			return output;
		}

		// Whether the output plays the clip frame that expected names at each position.
		static void check(const std::vector<float>& output, const std::function<int64_t(int64_t)>& expected)
		{
			size_t wrong = 0;
			size_t first = 0;
			for (size_t i = 0; i < output.size(); ++i)
				if (output[i] != ramp(expected(static_cast<int64_t>(i))) && wrong++ == 0)
					first = i;
			INFO(wrong << " wrong frames, the first at " << first);
			CHECK(wrong == 0);
		}

		Engine engine;
		Client client{engine};

	private:
		int64_t m_rendered = 0;
	};
}

TEST_CASE(
	"event.comp brings a lane's take to the front in a range, which plays and undoes as one step", "[integration]")
{
	const gnubase::test::TempDir dir;
	Fixture f;
	f.call("audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
	gnubase::test::Audio audio;
	audio.sampleRate = 48000;
	for (int64_t frame = 0; frame < 5000; ++frame)
		audio.samples.push_back(ramp(frame));
	const std::filesystem::path file = dir.path() / "ramp.wav";
	gnubase::test::writeWav(file, audio);
	const Json media = f.call("media.add", {{"path", file.u8string()}}).at("media");
	const auto clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
	const int64_t track = f.track("audio");
	// Two laps of a cycle recording: the second, added last, plays.
	const int64_t first = f.take(track, clip, 0, 1000, 0);
	const int64_t second = f.take(track, clip, 2000, 1000, 1);
	const auto secondTake = [](int64_t position) { return 2000 + position; };
	Fixture::check(f.render(1000), secondTake);

	// Comping the first lap's lane from 300 to 600 splits both takes there and plays the first one in between.
	const Json comped = f.call("event.comp", {{"track", track}, {"lane", 0}, {"start", 300}, {"end", 600}});
	REQUIRE(comped.at("events").size() == 1);
	const Json piece = f.object("audioEvents", comped.at("events").at(0));
	CHECK(piece.at("position") == 300);
	CHECK(piece.at("length") == 300);
	CHECK(piece.at("lane") == 0);
	const Json project = f.call("project.get");
	CHECK(project.at("audioEvents").size() == 6);
	int onFirstLane = 0;
	for (const Json& event : project.at("audioEvents"))
		onFirstLane += event.at("lane") == 0 ? 1 : 0;
	CHECK(onFirstLane == 3);
	const auto comp = [](int64_t position) { return position >= 300 && position < 600 ? position : 2000 + position; };
	Fixture::check(f.render(1000), comp);

	// One undo step takes it all back; redo brings the comp back.
	CHECK(f.call("history.get").at("steps").back().at("action") == "Comp");
	f.call("history.undo");
	CHECK(f.call("project.get").at("audioEvents").size() == 2);
	Fixture::check(f.render(1000), secondTake);
	f.call("history.redo");
	Fixture::check(f.render(1000), comp);

	// Bringing the first take's last piece to the front plays it from 600 on, too.
	f.call("history.undo");
	f.call("event.toFront", {{"events", {first}}});
	Fixture::check(f.render(1000), [](int64_t position) { return position; });
	f.call("event.toFront", {{"events", {second}}});
	Fixture::check(f.render(1000), secondTake);

	CHECK_THAT(f.fails("event.comp", {{"track", track}, {"lane", 0}, {"start", 600}, {"end", 600}}),
		ContainsSubstring("empty"));
	CHECK_THAT(f.fails("event.comp", {{"track", track}, {"lane", 5}, {"start", 0}, {"end", 600}}),
		ContainsSubstring("nothing"));
	CHECK_THAT(f.fails("event.comp", {{"track", 12345}, {"lane", 0}, {"start", 0}, {"end", 600}}),
		ContainsSubstring("no track"));
}

TEST_CASE("a split, the project file and undo keep where audio events are in the stack", "[integration]")
{
	const gnubase::test::TempDir dir;
	Fixture f;
	const int64_t track = f.track("audio");
	const Json media =
		f.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 48000}})
			.at("media");
	const auto clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
	const int64_t back = f.take(track, clip, 0, 1000, 0);
	const int64_t front = f.take(track, clip, 0, 1000, 1);
	CHECK(f.object("audioEvents", back).at("front") == 0); // its id orders it

	// The piece after a split comes right in front of its event, and the take in front gets a place after it.
	const Json pieces = f.call("event.split", {{"events", {back}}, {"positions", {500}}}).at("events");
	REQUIRE(pieces.size() == 1);
	CHECK(f.object("audioEvents", pieces.at(0)).at("front") == 0);
	CHECK(f.object("audioEvents", front).at("front").get<int64_t>() > pieces.at(0).get<int64_t>());

	f.call("event.toFront", {{"events", {back}}});
	const Json moved = f.object("audioEvents", back).at("front");
	CHECK(moved.get<int64_t>() > front);
	const std::string path = (dir.path() / "front.dawproject").u8string();
	f.call("project.save", {{"path", path}, {"embedMedia", false}});
	f.call("history.undo");
	CHECK(f.object("audioEvents", back).at("front") == 0);
	f.call("project.load", {{"path", path}});
	CHECK(f.object("audioEvents", back).at("front") == moved);
	// New events still come in front of it.
	CHECK(f.take(track, clip, 0, 100, 2) > moved.get<int64_t>());
}

TEST_CASE("a second split leaves what plays where the pieces of a split overlap, also after undo and loading",
	"[integration]")
{
	const gnubase::test::TempDir dir;
	Fixture f;
	f.call("audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
	gnubase::test::Audio audio;
	audio.sampleRate = 48000;
	for (int64_t frame = 0; frame < 5000; ++frame)
		audio.samples.push_back(ramp(frame));
	const std::filesystem::path file = dir.path() / "ramp.wav";
	gnubase::test::writeWav(file, audio);
	const Json media = f.call("media.add", {{"path", file.u8string()}}).at("media");
	const auto clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
	const int64_t track = f.track("audio");
	// An event from 0 to 2000, and a take added after it from 1300 to 1400, in front of it, playing frame 3000 on.
	const int64_t event = f.take(track, clip, 0, 2000, 0);
	const int64_t later = f.take(track, clip, 3000, 100, 1);
	f.call("event.move", {{"events", {later}}, {"delta", 1300}});
	// Split at 1000 and the right piece moved left over the left one to 500: it plays in the overlap, the take in
	// front of it from 1300 to 1400.
	const Json right = f.call("event.split", {{"events", {event}}, {"positions", {1000}}}).at("events").at(0);
	f.call("event.move", {{"events", {right}}, {"delta", -500}});
	const auto plays = [](int64_t position) -> int64_t
	{
		if (position < 500)
			return position;
		if (position >= 1300 && position < 1400)
			return 1700 + position;
		return position < 1500 ? position + 500 : -1;
	};
	Fixture::check(f.render(2000), plays);

	// Splitting the left piece before the overlap, and its piece after that inside it, changes nothing that plays.
	const Json after = f.call("event.split", {{"events", {event}}, {"positions", {250}}}).at("events").at(0);
	Fixture::check(f.render(2000), plays);
	f.call("event.split", {{"events", {after}}, {"positions", {700}}});
	Fixture::check(f.render(2000), plays);
	f.call("history.undo");
	f.call("history.undo");
	Fixture::check(f.render(2000), plays);
	f.call("history.redo");
	f.call("history.redo");
	Fixture::check(f.render(2000), plays);
	const std::string path = (dir.path() / "split.dawproject").u8string();
	f.call("project.save", {{"path", path}, {"embedMedia", false}});
	f.call("project.load", {{"path", path}});
	Fixture::check(f.render(2000), plays);
}

TEST_CASE("event.comp takes a piece of a clip at another sample rate that a split rounds past the range", "[unit]")
{
	Fixture f;
	const int64_t track = f.track("audio");
	const Json media =
		f.call("media.add", {{"path", "take.wav"}, {"sampleRate", 44100}, {"channels", 1}, {"length", 44100}})
			.at("media");
	const auto clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
	const int64_t back = f.take(track, clip, 0, 1000, 0);
	f.take(track, clip, 0, 1000, 1);

	// In a 48 kHz project the piece from 100 to 180 is 74 clip frames long and ends a sample after 180.
	const Json comped = f.call("event.comp", {{"track", track}, {"lane", 0}, {"start", 100}, {"end", 180}});
	REQUIRE(comped.at("events").size() == 1);
	const Json piece = f.object("audioEvents", comped.at("events").at(0));
	CHECK(piece.at("lane") == 0);
	CHECK(piece.at("position") == 100);
	CHECK(piece.at("front").get<int64_t>() > back);
}

TEST_CASE("event.comp leaves a locked take that crosses the range's edges where it is", "[unit]")
{
	Fixture f;
	const int64_t track = f.track("audio");
	const Json media =
		f.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 48000}})
			.at("media");
	const auto clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
	const int64_t locked = f.take(track, clip, 0, 1000, 0);
	f.call("event.set", {{"events", {locked}}, {"lock", true}});
	f.take(track, clip, 0, 1000, 1);

	// The locked take isn't split, so it isn't within 400 to 700 and doesn't come to the front whole.
	CHECK_THAT(f.fails("event.comp", {{"track", track}, {"lane", 0}, {"start", 400}, {"end", 700}}),
		ContainsSubstring("nothing"));
	CHECK(f.object("audioEvents", locked).at("front") == 0);
	// Wholly within the range it is taken.
	const Json comped = f.call("event.comp", {{"track", track}, {"lane", 0}, {"start", 0}, {"end", 1000}});
	REQUIRE(comped.at("events").size() == 1);
	CHECK(comped.at("events").at(0) == locked);
}

TEST_CASE("MIDI takes come to the front unmuted, muting the takes on other lanes they overlap", "[unit]")
{
	Fixture f;
	const int64_t track = f.track("midi", "musical");
	// A stacked cycle recording: the last lap plays, the others are muted.
	const int64_t first = f.part(track, 0, true);
	const int64_t second = f.part(track, 1, true);
	const int64_t third = f.part(track, 2, false);
	const int64_t other = f.track("midi", "musical");
	const int64_t elsewhere = f.part(other, 1, false);

	f.call("event.toFront", {{"events", {first}}});
	CHECK(f.object("midiParts", first).at("mute") == false);
	CHECK(f.object("midiParts", second).at("mute") == true);
	CHECK(f.object("midiParts", third).at("mute") == true);
	CHECK(f.object("midiParts", elsewhere).at("mute") == false);
	f.call("history.undo");
	CHECK(f.object("midiParts", first).at("mute") == true);
	CHECK(f.object("midiParts", third).at("mute") == false);

	// Comping the second lane in the second bar: the second take plays there, the last one around it.
	const Json comped =
		f.call("event.comp", {{"track", track}, {"lane", 1}, {"start", g_barSamples}, {"end", 2 * g_barSamples}});
	REQUIRE(comped.at("events").size() == 1);
	const Json project = f.call("project.get");
	int playing = 0;
	for (const Json& part : project.at("midiParts"))
	{
		if (part.at("track") != track)
			continue;
		const bool inRange = part.at("position") == g_bar;
		const bool plays = inRange ? part.at("lane") == 1 : part.at("lane") == 2;
		INFO("part " << part.at("id") << " on lane " << part.at("lane") << " at " << part.at("position"));
		CHECK(part.at("mute") == !plays);
		playing += plays ? 1 : 0;
	}
	CHECK(playing == 2);
	CHECK(project.at("midiParts").size() == 7);
}

TEST_CASE("an audio part's events keep their order in the stack when the part comes to the front, is copied or "
		  "pasted",
	"[unit]")
{
	Fixture f;
	const int64_t track = f.track("audio");
	const Json media =
		f.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 48000}})
			.at("media");
	const auto clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
	// Two takes in a part, told apart by their offsets: the first, brought to the front, plays.
	const int64_t first = f.take(track, clip, 0, 1000, 0);
	const int64_t second = f.take(track, clip, 2000, 1000, 1);
	f.call("event.toFront", {{"events", {first}}});
	const Json parts = f.call("event.toPart", {{"events", {first, second}}}).at("events");
	REQUIRE(parts.size() == 1);
	const auto part = parts.at(0).get<int64_t>();
	const auto stacked = [](const Json& event)
	{
		const auto own = event.at("front").get<int64_t>();
		return own != 0 ? own : event.at("id").get<int64_t>();
	};
	// The front place of the event at offset in the part, -1 for none.
	const auto placeIn = [&f, &stacked](const Json& inPart, int64_t offset)
	{
		const Json project = f.call("project.get");
		for (const Json& event : project.at("audioEvents"))
			if (event.at("part") == inPart && event.at("offset") == offset)
				return stacked(event);
		return int64_t{-1};
	};
	REQUIRE(placeIn(part, 0) > placeIn(part, 2000));

	// The part to the front: both come before every other event, the first still in front of the second.
	const int64_t later = f.take(track, clip, 4000, 1000, 2);
	f.call("event.toFront", {{"events", {part}}});
	CHECK(placeIn(part, 0) > placeIn(part, 2000));
	CHECK(placeIn(part, 2000) > later);

	// A copy of the part comes in front of what was added before it, as copied events do, keeping its events' order.
	const int64_t before = f.take(track, clip, 6000, 1000, 2);
	const Json copies = f.call("event.copy", {{"events", {part}}, {"delta", 0}}).at("events");
	REQUIRE(copies.size() == 1);
	CHECK(placeIn(copies.at(0), 0) > placeIn(copies.at(0), 2000));
	CHECK(placeIn(copies.at(0), 2000) > before);
	CHECK(f.object("audioEvents", before).at("front") == 0);

	// So does a pasted one.
	f.call("range.copy", {{"start", 0}, {"end", 1000}, {"tracks", {track}}});
	const Json pasted = f.call("range.paste", {{"position", 48000}}).at("events");
	const Json project = f.call("project.get");
	Json pastedPart;
	for (const Json& id : pasted)
		for (const Json& audioPart : project.at("audioParts"))
			if (audioPart.at("id") == id)
				pastedPart = id;
	REQUIRE(pastedPart.is_number());
	CHECK(placeIn(pastedPart, 0) > placeIn(pastedPart, 2000));
}

TEST_CASE("audioEvent.add, audioPart.add, midiPart.add and audioFile.import put what they add on a lane", "[unit]")
{
	const gnubase::test::TempDir dir;
	Fixture f;
	const int64_t audioTrack = f.track("audio");
	const int64_t midiTrack = f.track("midi", "musical");
	const Json media =
		f.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 48000}})
			.at("media");
	const auto clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();

	const Json event =
		f.call("audioEvent.add", {{"track", audioTrack}, {"clip", clip}, {"position", 0}, {"lane", 2}}).at("event");
	CHECK(event.at("lane") == 2);
	const Json audioPart =
		f.call("audioPart.add", {{"track", audioTrack}, {"position", 0}, {"length", 1000}, {"lane", 1}}).at("part");
	CHECK(audioPart.at("lane") == 1);
	const Json midiPart =
		f.call("midiPart.add", {{"track", midiTrack}, {"position", 0}, {"length", g_bar}, {"lane", 3}}).at("part");
	CHECK(midiPart.at("lane") == 3);
	CHECK(f.object("audioEvents", event.at("id")).at("lane") == 2);
	CHECK(f.object("midiParts", midiPart.at("id")).at("lane") == 3);
	// Without one, on lane 0; lanes go from 0 to 1000.
	CHECK(
		f.call("midiPart.add", {{"track", midiTrack}, {"position", 0}, {"length", g_bar}}).at("part").at("lane") == 0);
	CHECK_THAT(f.fails("midiPart.add", {{"track", midiTrack}, {"position", 0}, {"length", g_bar}, {"lane", -1}}),
		ContainsSubstring("lane"));
	CHECK_THAT(f.fails("audioPart.add", {{"track", audioTrack}, {"position", 0}, {"length", 1}, {"lane", 1001}}),
		ContainsSubstring("lane"));

	// Imported files go on the lane of the track they go on, not on the new tracks of the others.
	gnubase::test::Audio audio;
	audio.sampleRate = 48000;
	audio.samples.assign(4800, 0.25f);
	const std::filesystem::path file = dir.path() / "take.wav";
	gnubase::test::writeWav(file, audio);
	const Json imported = f.call("audioFile.import",
		{{"files", {file.u8string(), file.u8string()}}, {"track", audioTrack}, {"position", 0}, {"lane", 4}});
	REQUIRE(imported.at("events").size() == 2);
	CHECK(f.object("audioEvents", imported.at("events").at(0)).at("lane") == 4);
	CHECK(f.object("audioEvents", imported.at("events").at(0)).at("track") == audioTrack);
	CHECK(f.object("audioEvents", imported.at("events").at(1)).at("lane") == 0);
	const Json together = f.call("audioFile.import",
		{{"files", {file.u8string(), file.u8string()}}, {"track", audioTrack}, {"oneTrack", true}, {"lane", 1}});
	for (const Json& id : together.at("events"))
		CHECK(f.object("audioEvents", id).at("lane") == 1);
	// Without a track there is no lane to put them on.
	CHECK_THAT(f.fails("audioFile.import", {{"files", {file.u8string()}}, {"lane", 1}}), ContainsSubstring("track"));
}

TEST_CASE("takes copied, duplicated, pasted or brought to the front together keep their order in the stack", "[unit]")
{
	Fixture f;
	const int64_t track = f.track("audio");
	const Json media =
		f.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 48000}})
			.at("media");
	const auto clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
	// Two overlapping takes on one lane, told apart by their offsets: the first, brought to the front, plays.
	const int64_t first = f.take(track, clip, 0, 1000, 1);
	const int64_t second = f.take(track, clip, 2000, 1000, 1);
	f.call("event.toFront", {{"events", {first}}});
	// Whether, of the events at position, the first take's is in front of the second's.
	const auto firstInFront = [&f](int64_t position)
	{
		int64_t firstPlace = -1;
		int64_t secondPlace = -1;
		const Json project = f.call("project.get");
		for (const Json& event : project.at("audioEvents"))
		{
			if (event.at("position") != position)
				continue;
			const auto own = event.at("front").get<int64_t>();
			const int64_t place = own != 0 ? own : event.at("id").get<int64_t>();
			(event.at("offset") == 0 ? firstPlace : secondPlace) = place;
		}
		INFO("first " << firstPlace << ", second " << secondPlace);
		return firstPlace > 0 && secondPlace > 0 && firstPlace > secondPlace;
	};
	REQUIRE(firstInFront(0));

	// The copies come back in the order named: the first take's, then the second's.
	const auto inOrderNamed = [&f](const Json& copies)
	{
		return copies.size() == 2 && f.object("audioEvents", copies.at(0)).at("offset") == 0 &&
			f.object("audioEvents", copies.at(1)).at("offset") == 2000;
	};
	CHECK(inOrderNamed(f.call("event.copy", {{"events", {first, second}}, {"delta", 48000}}).at("events")));
	CHECK(firstInFront(48000));
	CHECK(inOrderNamed(f.call("event.duplicate", {{"events", {first, second}}}).at("events")));
	CHECK(firstInFront(1000));
	// Filling from 144000 to 145000 samples, one copy of the two.
	CHECK(inOrderNamed(
		f.call("event.fillLoop", {{"events", {first, second}}, {"start", 144000}, {"end", 145000}}).at("events")));
	CHECK(firstInFront(144000));
	f.call("range.copy", {{"start", 0}, {"end", 1000}, {"tracks", {track}}});
	f.call("range.paste", {{"position", 96000}});
	CHECK(firstInFront(96000));
	// Brought to the front together, and comped, they keep it too.
	f.call("event.toFront", {{"events", {first, second}}});
	CHECK(firstInFront(0));
	f.call("event.comp", {{"track", track}, {"lane", 1}, {"start", 0}, {"end", 1000}});
	CHECK(firstInFront(0));
}

TEST_CASE("overlapping pieces of a split copied or brought to the front keep their order in any order named", "[unit]")
{
	Fixture f;
	const int64_t track = f.track("audio");
	const Json media =
		f.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 48000}})
			.at("media");
	const auto clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
	// Split at 1000 and the right piece moved left over the left one to 500: the right piece plays in the overlap.
	const int64_t left = f.take(track, clip, 0, 2000, 0);
	const auto right =
		f.call("event.split", {{"events", {left}}, {"positions", {1000}}}).at("events").at(0).get<int64_t>();
	f.call("event.move", {{"events", {right}}, {"delta", -500}});
	// Whether, of the pieces at position (the left's) and 500 after it (the right's), the right one is in front.
	const auto rightInFront = [&f](int64_t position)
	{
		int64_t leftPlace = -1;
		int64_t rightPlace = -1;
		const Json project = f.call("project.get");
		for (const Json& event : project.at("audioEvents"))
		{
			const auto own = event.at("front").get<int64_t>();
			const int64_t place = own != 0 ? own : event.at("id").get<int64_t>();
			if (event.at("position") == position && event.at("offset") == 0)
				leftPlace = place;
			else if (event.at("position") == position + 500 && event.at("offset") == 1000)
				rightPlace = place;
		}
		INFO("left " << leftPlace << ", right " << rightPlace);
		return leftPlace > 0 && rightPlace > leftPlace;
	};
	REQUIRE(rightInFront(0));
	int64_t delta = 0;
	for (const Json& named : {Json{left, right}, Json{right, left}})
	{
		INFO("named " << named.dump());
		delta += 48000;
		f.call("event.copy", {{"events", named}, {"delta", delta}});
		CHECK(rightInFront(delta));
		f.call("event.toFront", {{"events", named}});
		CHECK(rightInFront(0));
	}
}

TEST_CASE("audio parts copied or brought to the front together keep their order in the stack", "[unit]")
{
	Fixture f;
	const int64_t track = f.track("audio");
	const Json media =
		f.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 48000}})
			.at("media");
	const auto clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
	// Two overlapping parts with a take each, told apart by their offsets; the first, brought to the front, plays.
	const auto partOf = [&f](int64_t take)
	{ return f.call("event.toPart", {{"events", {take}}}).at("events").at(0).get<int64_t>(); };
	const int64_t first = partOf(f.take(track, clip, 0, 1000, 0));
	const int64_t second = partOf(f.take(track, clip, 2000, 1000, 1));
	f.call("event.toFront", {{"events", {first}}});
	// Whether, of the takes in parts at position, the first part's is in front of the second's.
	const auto firstInFront = [&f](int64_t position)
	{
		int64_t firstPlace = -1;
		int64_t secondPlace = -1;
		const Json project = f.call("project.get");
		for (const Json& event : project.at("audioEvents"))
		{
			if (event.at("position") != position || event.at("part") == 0)
				continue;
			const auto own = event.at("front").get<int64_t>();
			const int64_t place = own != 0 ? own : event.at("id").get<int64_t>();
			(event.at("offset") == 0 ? firstPlace : secondPlace) = place;
		}
		INFO("first " << firstPlace << ", second " << secondPlace);
		return firstPlace > 0 && secondPlace > 0 && firstPlace > secondPlace;
	};
	REQUIRE(firstInFront(0));

	f.call("event.copy", {{"events", {first, second}}, {"delta", 48000}});
	CHECK(firstInFront(48000));
	f.call("event.toFront", {{"events", {first, second}}});
	CHECK(firstInFront(0));
}

TEST_CASE("a duplicated track's takes come in front, keeping their order in the stack", "[unit]")
{
	Fixture f;
	const int64_t track = f.track("audio");
	const Json media =
		f.call("media.add", {{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 48000}})
			.at("media");
	const auto clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
	// Two overlapping takes, the first brought to the front: on the copy the first plays too.
	const int64_t first = f.take(track, clip, 0, 1000, 0);
	f.take(track, clip, 2000, 1000, 1);
	f.call("event.toFront", {{"events", {first}}});
	const Json copy = f.call("track.duplicate", {{"track", track}}).at("track").at("id");
	int64_t firstPlace = -1;
	int64_t secondPlace = -1;
	const Json project = f.call("project.get");
	for (const Json& event : project.at("audioEvents"))
	{
		if (event.at("track") != copy)
			continue;
		CHECK(event.at("front") == 0); // in front of every event added before, as added last
		(event.at("offset") == 0 ? firstPlace : secondPlace) = event.at("id").get<int64_t>();
	}
	CHECK(secondPlace > 0);
	CHECK(firstPlace > secondPlace);
}

TEST_CASE("a muted take in front silences what lies behind it only with editing.treatMutedAudioLikeDeleted off",
	"[integration]")
{
	const gnubase::test::TempDir dir;
	Fixture f;
	f.call("audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
	gnubase::test::Audio audio;
	audio.sampleRate = 48000;
	for (int64_t frame = 0; frame < 5000; ++frame)
		audio.samples.push_back(ramp(frame));
	const std::filesystem::path file = dir.path() / "ramp.wav";
	gnubase::test::writeWav(file, audio);
	const Json media = f.call("media.add", {{"path", file.u8string()}}).at("media");
	const auto clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
	const int64_t track = f.track("audio");
	// Two takes over the same place: the second, added last, is in front. Mute it.
	f.take(track, clip, 0, 1000, 0);
	const int64_t front = f.take(track, clip, 2000, 1000, 1);
	f.call("event.set", {{"events", {front}}, {"mute", true}});

	// The default treats the muted take as deleted, so the take behind it plays.
	const std::vector<float> behind = f.render(1000);
	Fixture::check(behind, [](int64_t position) { return position; });

	// With the preference off (reference manual p. 228), the muted take keeps its place and silences the one behind.
	f.call("settings.set", {{"key", "editing.treatMutedAudioLikeDeleted"}, {"value", false}});
	const std::vector<float> silenced = f.render(1000);
	CHECK(std::all_of(silenced.begin(), silenced.end(), [](float sample) { return sample == 0.0f; }));
	CHECK(silenced != behind);

	// And back on, the take behind plays again.
	f.call("settings.set", {{"key", "editing.treatMutedAudioLikeDeleted"}, {"value", true}});
	Fixture::check(f.render(1000), [](int64_t position) { return position; });
}
