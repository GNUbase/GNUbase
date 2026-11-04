// SPDX-License-Identifier: GPL-3.0-or-later
// Lanes and comping (#99, reference manual pp. 225-228): event.toFront brings takes to the front, event.comp brings a
// lane's takes to the front in a range, and what plays follows. docs/features/comping.md describes them.
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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

	// The piece after a split stays where the event was in the stack, behind the take in front.
	const Json pieces = f.call("event.split", {{"events", {back}}, {"positions", {500}}}).at("events");
	REQUIRE(pieces.size() == 1);
	CHECK(f.object("audioEvents", pieces.at(0)).at("front") == back);

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
