// SPDX-License-Identifier: GPL-3.0-or-later
// Audio quantizing (#145, docs/features/audio-quantize.md, reference manual pp. 367-380): audio event starts (their
// snap points) to the grid, warp quantizing of warp markers, and several audio tracks sliced at the hitpoints of
// the track with the highest priority, quantized and crossfaded so that they stay phase coherent.
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;
using engine::ObjectId;

namespace
{
	constexpr double g_pi = 3.14159265358979323846;
	constexpr int64_t g_quarter = 24000; // samples per quarter at 120 BPM and 48 kHz

	Json run(engine::Client& client, const std::string& command, Json parameters = Json::object())
	{
		const engine::Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(engine::Client& client, const std::string& command, Json parameters = Json::object())
	{
		const engine::Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	// 5 ms of 1.5 kHz under a Hann window centred on each of the frames, mono at 48 kHz, scaled by gain.
	test::Audio clicks(const std::vector<int64_t>& at, int64_t frames, float gain = 1.0F)
	{
		test::Audio audio;
		audio.samples.assign(static_cast<size_t>(frames), 0.0F);
		constexpr int64_t burst = 240;
		for (const int64_t click : at)
			for (int64_t i = 0; i < burst; ++i)
			{
				const double t = (static_cast<double>(i) + 0.5) / burst;
				const double envelope = 0.5 - (0.5 * std::cos(2.0 * g_pi * t));
				const double phase = 2.0 * g_pi * 1500.0 * static_cast<double>(i) / 48000.0;
				audio.samples[static_cast<size_t>(click - (burst / 2) + i)] =
					gain * static_cast<float>(0.8 * envelope * std::sin(phase));
			}
		return audio;
	}

	// The centre of the energy within 50 ms of expected.
	double centre(const std::vector<float>& samples, double expected)
	{
		const auto from = std::max<int64_t>(0, std::llround(expected) - 2400);
		const auto to = std::min<int64_t>(static_cast<int64_t>(samples.size()), std::llround(expected) + 2400);
		double energy = 0.0;
		double moment = 0.0;
		for (int64_t i = from; i < to; ++i)
		{
			const double square =
				static_cast<double>(samples[static_cast<size_t>(i)]) * samples[static_cast<size_t>(i)];
			energy += square;
			moment += square * static_cast<double>(i);
		}
		REQUIRE(energy > 0.0);
		return moment / energy;
	}

	// Checks that a click is within tolerance frames of each of the frames.
	void checkClicks(const std::vector<float>& samples, const std::vector<double>& expected, double tolerance = 480.0)
	{
		for (const double at : expected)
		{
			const double offset = centre(samples, at) - at;
			CAPTURE(at, offset);
			CHECK(std::abs(offset) <= tolerance);
		}
	}

	struct Loop
	{
		ObjectId track = 0;
		ObjectId clip = 0;
		ObjectId event = 0;
	};

	// The audio as a clip, and an event of it at position, in the track's time base, on a new audio track.
	Loop addLoop(engine::Client& client, const test::TempDir& dir, const std::string& name, const test::Audio& audio,
		int64_t position = 0, const std::string& timeBase = "linear")
	{
		const std::string file = (dir.path() / (name + ".wav")).u8string();
		test::writeWav(file, audio);
		Loop loop;
		loop.track = run(client, "track.add", {{"type", "audio"}, {"timeBase", timeBase}, {"channels", 1}})
						 .at("track")
						 .at("id")
						 .get<ObjectId>();
		const Json media = run(client, "media.add", {{"path", file}}).at("media");
		loop.clip = run(client, "clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<ObjectId>();
		loop.event = run(client, "audioEvent.add",
			{{"track", loop.track}, {"clip", loop.clip}, {"position", position}, {"offset", 0},
				{"length", static_cast<int64_t>(audio.frames())}})
						 .at("event")
						 .at("id")
						 .get<ObjectId>();
		return loop;
	}

	// Hitpoints that count at the clip frames.
	void setHitpoints(engine::Client& client, ObjectId clip, const std::vector<int64_t>& at)
	{
		Json hitpoints = Json::array();
		for (const int64_t position : at)
			hitpoints.push_back({{"position", position}, {"peak", -3.0}, {"intensity", 1.0}});
		run(client, "clip.setHitpoints", {{"clip", clip}, {"hitpoints", std::move(hitpoints)}});
	}

	std::vector<float> render(engine::Client& client, int64_t frames)
	{
		run(client, "audio.setDevice", {{"blockSize", 256}, {"inputChannels", 0}, {"captureSeconds", 6}});
		run(client, "transport.stop");
		run(client, "transport.locate", {{"position", 0}});
		run(client, "transport.play");
		run(client, "audio.render", {{"frames", frames}});
		run(client, "transport.stop");
		return run(client, "audio.capture", {{"start", 0}, {"frames", frames}, {"channel", 0}})
			.at("samples")
			.get<std::vector<float>>();
	}

	Json byId(engine::Client& client, const char* list, ObjectId id)
	{
		const Json project = run(client, "project.get");
		for (const Json& each : project.at(list))
			if (each.at("id") == id)
				return each;
		return nullptr;
	}

	int64_t positionOf(engine::Client& client, ObjectId event)
	{
		const Json found = byId(client, "audioEvents", event);
		REQUIRE(found.is_object());
		return found.at("position").get<int64_t>();
	}

	Json markersOf(engine::Client& client, ObjectId clip)
	{
		const Json found = byId(client, "clips", clip);
		REQUIRE(found.is_object());
		return found.at("warp").at("markers");
	}

	// The events of a track by position: their positions, offsets and lengths.
	std::vector<std::vector<int64_t>> eventsOn(engine::Client& client, ObjectId track)
	{
		std::vector<std::vector<int64_t>> events;
		const Json project = run(client, "project.get");
		for (const Json& event : project.at("audioEvents"))
			if (event.at("track") == track)
				events.push_back({event.at("position").get<int64_t>(), event.at("offset").get<int64_t>(),
					event.at("length").get<int64_t>()});
		std::sort(events.begin(), events.end());
		return events;
	}

	// The starts of a track's events, by position.
	std::vector<int64_t> startsOn(engine::Client& client, ObjectId track)
	{
		std::vector<int64_t> starts;
		for (const std::vector<int64_t>& event : eventsOn(client, track))
			starts.push_back(event[0]);
		return starts;
	}

	// How many of the project's audio events crossfade from another one.
	size_t crossfades(engine::Client& client)
	{
		size_t found = 0;
		const Json project = run(client, "project.get");
		for (const Json& event : project.at("audioEvents"))
			if (event.at("crossfade").is_object())
				++found;
		return found;
	}

	Json crossfadeOf(engine::Client& client, ObjectId event)
	{
		return byId(client, "audioEvents", event).at("crossfade");
	}

	// One state of an event's quantize origin as the project gives it, without a crossfade in it.
	Json originState(int64_t position, int64_t offset, int64_t length)
	{
		return {{"position", position}, {"offset", offset}, {"length", length}, {"crossfade", nullptr}};
	}

	// A new linear audio track, a clip of the audio, and an event of length frames of it at position playing the
	// clip from offset.
	struct Track
	{
		ObjectId track = 0;
		ObjectId clip = 0;

		ObjectId add(engine::Client& client, int64_t position, int64_t offset, int64_t length) const
		{
			return run(client, "audioEvent.add",
				{{"track", track}, {"clip", clip}, {"position", position}, {"offset", offset}, {"length", length}})
				.at("event")
				.at("id")
				.get<ObjectId>();
		}
	};

	Track addTrack(engine::Client& client, const test::TempDir& dir, const std::string& name, const test::Audio& audio)
	{
		const std::string file = (dir.path() / (name + ".wav")).u8string();
		test::writeWav(file, audio);
		Track added;
		added.track = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 1}})
						  .at("track")
						  .at("id")
						  .get<ObjectId>();
		const ObjectId media = run(client, "media.add", {{"path", file}}).at("media").at("id").get<ObjectId>();
		added.clip = run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
		return added;
	}

	std::string lastStep(engine::Client& client)
	{
		return run(client, "history.get").at("steps").back().at("action").get<std::string>();
	}

	// The track with copies of a track's events, made as how says: the track's duplicate, the track itself after
	// duplicating its version, or the track imported from a project file it is saved in.
	ObjectId copyOf(engine::Client& client, const test::TempDir& dir, ObjectId track, const std::string& how)
	{
		if (how == "track.duplicate")
			return run(client, how, {{"track", track}}).at("track").at("id").get<ObjectId>();
		if (how == "trackVersion.duplicate")
		{
			run(client, how, {{"tracks", {track}}});
			return track;
		}
		const std::string file = (dir.path() / "copied.dawproject").u8string();
		run(client, "project.save", {{"path", file}, {"embedMedia", false}});
		return run(client, "track.import", {{"path", file}}).at("tracks").at(0).get<ObjectId>();
	}

	// The ids of a track's audio events, by position.
	std::vector<ObjectId> idsOn(engine::Client& client, ObjectId track)
	{
		std::vector<std::pair<int64_t, ObjectId>> events;
		const Json project = run(client, "project.get");
		for (const Json& event : project.at("audioEvents"))
			if (event.at("track") == track)
				events.emplace_back(event.at("position").get<int64_t>(), event.at("id").get<ObjectId>());
		std::sort(events.begin(), events.end());
		std::vector<ObjectId> ids;
		ids.reserve(events.size());
		for (const auto& [position, id] : events)
			ids.push_back(id);
		return ids;
	}
}

TEST_CASE("quantizing audio events moves their snap points to the grid", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	run(client, "quantize.set", {{"preset", "1/4"}});
	const Loop late = addLoop(client, dir, "late", clicks({1000}, g_quarter), g_quarter + 2000);
	// Its snap point is 1000 frames in: it plays at 51000, so the event goes to 47000 for it to play on beat 3.
	const Loop snapped = addLoop(client, dir, "snapped", clicks({1000}, g_quarter), 50000);
	run(client, "event.setSnapPoint", {{"events", {snapped.event}}, {"position", 51000}});

	Json result = run(client, "quantize.apply", {{"events", {late.event, snapped.event}}});
	CHECK(lastStep(client) == "Quantize");
	CHECK(result.at("events").size() == 2);
	CHECK(positionOf(client, late.event) == g_quarter);
	CHECK(positionOf(client, snapped.event) == 47000);

	SECTION("quantizing again starts from the original positions, unless it is soft")
	{
		run(client, "quantize.apply", {{"events", {late.event}}, {"settings", {{"strength", 0.5}}}});
		CHECK(positionOf(client, late.event) == g_quarter + 1000);
		run(client, "quantize.apply", {{"events", {late.event}}, {"settings", {{"strength", 0.5}, {"soft", true}}}});
		CHECK(positionOf(client, late.event) == g_quarter + 500);
	}

	SECTION("reset puts back the original positions, except of events moved since")
	{
		run(client, "event.move", {{"events", {snapped.event}}, {"delta", 100}});
		result = run(client, "quantize.reset", {{"events", {late.event, snapped.event}}});
		CHECK(lastStep(client) == "Reset quantize");
		CHECK(result.at("events").size() == 1);
		CHECK(positionOf(client, late.event) == g_quarter + 2000);
		CHECK(positionOf(client, snapped.event) == 47100);
		run(client, "history.undo");
		CHECK(positionOf(client, late.event) == g_quarter);
	}

	SECTION("the events of an audio part")
	{
		const Json part = run(client, "event.toPart", {{"events", {late.event}}}).at("events").at(0);
		run(client, "quantize.reset", {{"events", {late.event}}});
		CHECK(positionOf(client, late.event) == g_quarter + 2000);
		run(client, "quantize.apply", {{"audioParts", {part}}});
		CHECK(positionOf(client, late.event) == g_quarter);
		run(client, "quantize.reset", {{"audioParts", {part}}});
		CHECK(positionOf(client, late.event) == g_quarter + 2000);
	}

	SECTION("locked events and those outside the catch range stay")
	{
		run(client, "quantize.reset", {{"events", {late.event, snapped.event}}});
		run(client, "event.set", {{"events", {late.event}}, {"lock", true}});
		run(client, "quantize.apply",
			{{"events", {late.event, snapped.event}}, {"settings", {{"catchRange", time::g_ticksPerQuarter / 16}}}});
		CHECK(positionOf(client, late.event) == g_quarter + 2000);
		CHECK(positionOf(client, snapped.event) == 50000);
	}

	SECTION("the original positions are saved with the project")
	{
		const std::string file = (dir.path() / "quantized.dawproject").u8string();
		run(client, "project.save", {{"path", file}, {"embedMedia", false}});
		run(client, "project.new");
		run(client, "project.load", {{"path", file}});
		run(client, "quantize.reset", {{"events", {late.event}}});
		CHECK(positionOf(client, late.event) == g_quarter + 2000);
	}

	CHECK_THAT(reject(client, "quantize.apply", {{"events", {999}}}), ContainsSubstring("no audio event 999"));
	CHECK_THAT(reject(client, "quantize.apply", {{"audioParts", {999}}}), ContainsSubstring("no audio part 999"));
	CHECK_THAT(reject(client, "quantize.apply"), ContainsSubstring("give"));
}

TEST_CASE("splitting a quantized audio event keeps both pieces resettable", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	run(client, "quantize.set", {{"preset", "1/4"}});
	// Quantizing takes the event from 2000 to the grid at 0; splitting it at 12000 leaves the piece after the split
	// where it was before quantizing, 14000, so that resetting puts both pieces back.
	const Loop loop = addLoop(client, dir, "loop", clicks({1000}, 4 * g_quarter), 2000);
	run(client, "quantize.apply", {{"events", {loop.event}}});
	REQUIRE(positionOf(client, loop.event) == 0);
	const Json pieces = run(client, "event.split", {{"events", {loop.event}}, {"positions", {12000}}}).at("events");
	REQUIRE(pieces.size() == 1);
	const auto right = pieces.at(0).get<ObjectId>();
	CHECK(byId(client, "audioEvents", right).at("quantizeOrigin") ==
		Json({{"original", originState(14000, 12000, 84000)}, {"quantized", originState(12000, 12000, 84000)}}));

	run(client, "quantize.reset", {{"events", {loop.event, right}}});
	CHECK(positionOf(client, loop.event) == 2000);
	CHECK(positionOf(client, right) == 14000);
	CHECK(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{2000, 0, 12000}, {14000, 12000, 84000}});
}

TEST_CASE("splitting a quantized audio event of a musical track keeps both pieces resettable", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// A musical track whose tempo halves at quarter 1. A piece's position is in ticks, but its audio keeps its length
	// in samples (#414), so how far the piece after a split lies from the event's start depends on the tempo there:
	// the origin has to be worked out through samples, not through the ticks quantizing moved the event by.
	run(client, "project.set", {{"tempoTrackActive", true}});
	run(client, "tempo.add", {{"position", time::g_ticksPerQuarter}, {"bpm", 60.0}, {"ramp", false}});
	run(client, "quantize.set", {{"preset", "1/4"}});
	const int64_t quarter = time::g_ticksPerQuarter;
	const int64_t original = (6 * quarter) / 10;        // 0.6 quarter, sample 14400
	const int64_t split = (1625 * quarter) / 1000;      // 1.625 quarters, sample 54000
	const int64_t afterSplit = (1425 * quarter) / 1000; // 1.425 quarters, sample 44400
	// The event starts at 0.6 quarter and quantizing takes it to quarter 1, sample 24000.
	const Loop loop = addLoop(client, dir, "loop", clicks({1000}, 4 * g_quarter), original, "musical");
	run(client, "quantize.apply", {{"events", {loop.event}}});
	REQUIRE(positionOf(client, loop.event) == quarter);

	// Split 30000 samples in: the piece after it was 30000 samples after the event's original start.
	const Json pieces = run(client, "event.split", {{"events", {loop.event}}, {"positions", {54000}}}).at("events");
	REQUIRE(pieces.size() == 1);
	const auto right = pieces.at(0).get<ObjectId>();
	CHECK(byId(client, "audioEvents", right).at("quantizeOrigin") ==
		Json({{"original", originState(afterSplit, 30000, 66000)}, {"quantized", originState(split, 30000, 66000)}}));

	run(client, "quantize.reset", {{"events", {loop.event, right}}});
	CHECK(positionOf(client, loop.event) == original);
	CHECK(positionOf(client, right) == afterSplit);
	// The pieces tile again: the first plays samples 14400 to 44400, the second 44400 to 110400.
	CHECK(eventsOn(client, loop.track) ==
		std::vector<std::vector<int64_t>>{{original, 0, 30000}, {afterSplit, 30000, 66000}});
}

TEST_CASE("Warp quantizing aligns warp markers with the grid by time stretch", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 256}, {"inputChannels", 0}, {"captureSeconds", 6}});
	// Played on 1/8 notes, with the quarters early or late: 25000, 47000 and 73000 are beats 2 to 4 a little off,
	// 36500 the eighth between beats 2 and 3. The click at 1000 has no hitpoint.
	const Loop loop = addLoop(client, dir, "loop", clicks({1000, 25000, 36500, 47000, 73000}, 4 * g_quarter));
	setHitpoints(client, loop.clip, {25000, 36500, 47000, 73000});
	Json result = run(client, "quantize.set", {{"preset", "1/4"}, {"settings", {{"warpQuantize", true}}}});
	CHECK(result.at("settings").at("warpQuantize") == true);

	// Without warp markers, quantizing makes them of the hitpoints. Those nearest to a quarter go onto it; the eighth
	// between keeps its place relative to them (reference manual p. 370).
	result = run(client, "quantize.apply", {{"events", {loop.event}}});
	CHECK(lastStep(client) == "Quantize");
	CHECK(result.at("clips") == Json::array({loop.clip}));
	CHECK(positionOf(client, loop.event) == 0);
	const Json markers = markersOf(client, loop.clip);
	REQUIRE(markers.size() == 4);
	const double eighth = g_quarter + (11500.0 * 24000.0 / 22000.0);
	checkClicks(render(client, 4 * g_quarter), {960.0, 24000.0, eighth, 48000.0, 72000.0});

	SECTION("reset removes the markers quantizing made")
	{
		run(client, "quantize.reset", {{"events", {loop.event}}});
		CHECK(markersOf(client, loop.clip).empty());
		checkClicks(render(client, 4 * g_quarter), {1000.0, 25000.0, 36500.0, 47000.0, 73000.0});
	}

	SECTION("a locked event's markers stay")
	{
		run(client, "event.set", {{"events", {loop.event}}, {"lock", true}});
		CHECK(run(client, "quantize.reset", {{"events", {loop.event}}}).at("clips").empty());
		CHECK(markersOf(client, loop.clip) == markers);
	}

	SECTION("quantizing again starts from the original markers")
	{
		run(client, "quantize.apply", {{"events", {loop.event}}, {"settings", {{"strength", 0.0}}}});
		const Json same = markersOf(client, loop.clip);
		for (const Json& marker : same)
			CHECK(std::abs(marker.at("target").get<int64_t>() - marker.at("position").get<int64_t>()) <= 1);
	}

	SECTION("markers there already are quantized, and stay after a reset")
	{
		run(client, "quantize.reset", {{"events", {loop.event}}});
		run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 47000}});
		run(client, "quantize.apply", {{"events", {loop.event}}});
		REQUIRE(markersOf(client, loop.clip).size() == 1);
		checkClicks(render(client, 4 * g_quarter), {25000.0 * 48000.0 / 47000.0, 48000.0});
		run(client, "quantize.reset", {{"events", {loop.event}}});
		CHECK(markersOf(client, loop.clip) == Json::array({{{"position", 47000}, {"target", 47000}}}));
	}
}

TEST_CASE("Warp quantizing a musical clip follows the project's tempo", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 256}, {"inputChannels", 0}, {"captureSeconds", 6}});
	// The loop above, but its clip is in musical mode at 120 BPM while the project runs at 60: every clip frame takes
	// two project samples, so finding the markers' targets has to go through the warp.
	const Loop loop = addLoop(client, dir, "musical", clicks({1000, 25000, 36500, 47000, 73000}, 4 * g_quarter));
	setHitpoints(client, loop.clip, {25000, 36500, 47000, 73000});
	run(client, "project.set", {{"tempo", 60.0}});
	run(client, "clip.setWarp", {{"clip", loop.clip}, {"musical", true}});
	run(client, "clip.setWarp", {{"clip", loop.clip}, {"tempo", 120.0}});
	run(client, "quantize.set", {{"preset", "1/4"}, {"settings", {{"warpQuantize", true}}}});
	checkClicks(render(client, 8 * g_quarter), {2000.0, 50000.0, 73000.0, 94000.0, 146000.0});

	// A quarter is 48000 samples now, so the clicks land on twice the places the same clip quantizes to at 120 BPM.
	run(client, "quantize.apply", {{"events", {loop.event}}});
	const double eighth = (2 * g_quarter) + (23000.0 * 48000.0 / 44000.0);
	checkClicks(render(client, 8 * g_quarter), {1920.0, 48000.0, eighth, 96000.0, 144000.0});

	run(client, "quantize.reset", {{"events", {loop.event}}});
	CHECK(markersOf(client, loop.clip).empty());
	checkClicks(render(client, 8 * g_quarter), {2000.0, 50000.0, 73000.0, 94000.0, 146000.0});
}

TEST_CASE("quantizing several audio tracks keeps them phase coherent", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 256}, {"inputChannels", 0}, {"captureSeconds", 6}});
	// A drum loop played a little off the beat, on a close microphone and, polarity inverted, on another one: their
	// sum is silence as long as both tracks stay in phase.
	const std::vector<int64_t> played{6200, 25600, 47300, 72800};
	const Loop close = addLoop(client, dir, "close", clicks(played, 4 * g_quarter));
	const Loop room = addLoop(client, dir, "room", clicks(played, 4 * g_quarter, -1.0F));
	setHitpoints(client, close.clip, played);
	// The room track's own hitpoints: a little later than the close one's, and one more alone.
	setHitpoints(client, room.clip, {6300, 25700, 60000, 72900});

	// The close track's hitpoints come first; of the room's only the one at 60000 has none of them within the range.
	Json result = run(client, "quantize.slice",
		{{"events", {close.event, room.event}},
			{"priorities", {{{"track", close.track}, {"priority", 2}}, {{"track", room.track}, {"priority", 1}}}},
			{"range", 20.0}, {"offset", 5.0}});
	CHECK(lastStep(client) == "Slice");
	CHECK(result.at("positions") == Json::array({6200, 25600, 47300, 60000, 72800}));
	// Both tracks are cut 5 ms (240 samples) before each of them: the same slices at the same places.
	const std::vector<std::vector<int64_t>> slices = eventsOn(client, close.track);
	REQUIRE(slices.size() == 6);
	CHECK(slices == eventsOn(client, room.track));
	CHECK(slices[2][0] == 25600 - 240);
	CHECK(result.at("events").size() == 12);

	// Quantized, the slices move to the grid by their snap points, the hitpoints: on both tracks alike.
	run(client, "quantize.set", {{"preset", "1/8"}});
	run(client, "quantize.apply", {{"events", result.at("events")}});
	const std::vector<std::vector<int64_t>> quantized = eventsOn(client, close.track);
	CHECK(quantized == eventsOn(client, room.track));
	CHECK(quantized[1][0] == (g_quarter / 2) - 240);
	CHECK(quantized[2][0] == g_quarter - 240);
	CHECK(quantized[3][0] == (2 * g_quarter) - 240);
	CHECK(quantized[4][0] == 60000 - 240);

	// Crossfade closes the gaps and removes the overlaps, with crossfades over them.
	Json events = Json::array();
	const Json project = run(client, "project.get");
	for (const Json& event : project.at("audioEvents"))
		events.push_back(event.at("id"));
	run(client, "quantize.crossfade", {{"events", events}, {"length", 2.0}});
	CHECK(lastStep(client) == "Crossfade");
	const std::vector<std::vector<int64_t>> crossfaded = eventsOn(client, close.track);
	CHECK(crossfaded == eventsOn(client, room.track));
	for (size_t i = 1; i < crossfaded.size(); ++i)
	{
		// Each slice ends 2 ms after the next one starts.
		CAPTURE(i);
		CHECK(crossfaded[i - 1][0] + crossfaded[i - 1][2] == crossfaded[i][0] + 96);
	}

	// The tracks cancel each other out: phase coherent.
	const std::vector<float> sum = render(client, 4 * g_quarter);
	float loudest = 0.0F;
	for (const float sample : sum)
		loudest = std::max(loudest, std::abs(sample));
	CHECK(loudest < 1e-6F);
	// Alone, the close track plays the hits on the beats.
	Json roomEvents = Json::array();
	const Json mixed = run(client, "project.get");
	for (const Json& event : mixed.at("audioEvents"))
		if (event.at("track") == room.track)
			roomEvents.push_back(event.at("id"));
	run(client, "event.set", {{"events", roomEvents}, {"mute", true}});
	checkClicks(render(client, 4 * g_quarter), {12000.0, 24000.0, 48000.0, 72000.0}, 2.0);

	CHECK_THAT(reject(client, "quantize.slice",
				   {{"events", {close.event}}, {"priorities", {{{"track", close.track}, {"priority", 0}}}}}),
		ContainsSubstring("no hitpoints"));
	CHECK_THAT(reject(client, "quantize.slice",
				   {{"events", {close.event}}, {"priorities", {{{"track", close.track}, {"priority", 6}}}}}),
		ContainsSubstring("0 to 5"));
	CHECK_THAT(reject(client, "quantize.crossfade", {{"events", {999}}}), ContainsSubstring("no audio event 999"));
}

TEST_CASE("The slice range merges hitpoints of different tracks only", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// A flam on the priority track, 5 ms apart, and one hitpoint of the other track between them. The range says when
	// two hitpoints on different tracks mark the same beat (reference manual pp. 378, 380), so the flam stays two
	// positions and the other track's hitpoint merges with the first of them.
	const std::vector<int64_t> flam{25000, 25100, 25240};
	const Loop drums = addLoop(client, dir, "drums", clicks(flam, 2 * g_quarter));
	const Loop room = addLoop(client, dir, "room", clicks(flam, 2 * g_quarter));
	setHitpoints(client, drums.clip, {25000, 25240});
	setHitpoints(client, room.clip, {25100});

	const Json result = run(client, "quantize.slice",
		{{"events", {drums.event, room.event}},
			{"priorities", {{{"track", drums.track}, {"priority", 2}}, {{"track", room.track}, {"priority", 1}}}},
			{"range", 20.0}});
	CHECK(result.at("positions") == Json::array({25000, 25240}));
	// Both tracks are cut at both of them.
	const std::vector<std::vector<int64_t>> slices = eventsOn(client, drums.track);
	REQUIRE(slices.size() == 3);
	CHECK(slices == eventsOn(client, room.track));
}

TEST_CASE("Slicing clears the snap point of the slices it doesn't give one", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Each track's event snaps at a place of its own before the first hitpoint. The slice before it starts at no
	// position, so it gets no snap point: keeping the one event.split copied would move the two tracks apart there.
	const std::vector<int64_t> played{25600, 47300};
	const Loop drums = addLoop(client, dir, "drums", clicks(played, 4 * g_quarter), 2 * g_quarter);
	const Loop room = addLoop(client, dir, "room", clicks(played, 4 * g_quarter), 2 * g_quarter);
	setHitpoints(client, drums.clip, played);
	setHitpoints(client, room.clip, played);
	run(client, "event.setSnapPoint", {{"events", {drums.event}}, {"position", (2 * g_quarter) + 1000}});
	run(client, "event.setSnapPoint", {{"events", {room.event}}, {"position", (2 * g_quarter) + 5000}});

	const Json result = run(client, "quantize.slice", {{"events", {drums.event, room.event}}});
	REQUIRE(result.at("positions") == Json::array({(2 * g_quarter) + 25600, (2 * g_quarter) + 47300}));
	// Neither first slice, which starts at no position, keeps the snap point its event had.
	size_t first = 0;
	const Json project = run(client, "project.get");
	for (const Json& event : project.at("audioEvents"))
		if (event.at("position").get<int64_t>() == 2 * g_quarter)
		{
			CHECK(event.at("snapPoint").is_null());
			++first;
		}
	REQUIRE(first == 2);

	// Without a snap point the slices before the first position stay where they are, on both tracks alike.
	run(client, "quantize.set", {{"preset", "1/4"}});
	run(client, "quantize.apply", {{"events", result.at("events")}});
	const std::vector<std::vector<int64_t>> quantized = eventsOn(client, drums.track);
	REQUIRE(quantized.size() == 3);
	CHECK(quantized == eventsOn(client, room.track));
	CHECK(quantized[0][0] == 2 * g_quarter);
}

TEST_CASE("soft and rough quantizing move the slices of several tracks alike", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Two tracks of the same loop sliced at the same positions: the randomizing is seeded by position, so the slices
	// at the same place move by the same amount, and soft quantizing goes on from there on both tracks alike.
	const std::vector<int64_t> played{6200, 25600, 47300, 72800};
	const Loop drums = addLoop(client, dir, "drums", clicks(played, 4 * g_quarter));
	const Loop room = addLoop(client, dir, "room", clicks(played, 4 * g_quarter));
	setHitpoints(client, drums.clip, played);
	setHitpoints(client, room.clip, played);
	const Json sliced = run(client, "quantize.slice", {{"events", {drums.event, room.event}}, {"offset", 5.0}});

	// Rough quantizing to 1/8, by up to an eighth of a quarter (750 samples) off the grid.
	run(client, "quantize.set", {{"preset", "1/8"}, {"settings", {{"randomize", time::g_ticksPerQuarter / 32}}}});
	run(client, "quantize.apply", {{"events", sliced.at("events")}});
	const std::vector<int64_t> rough = startsOn(client, drums.track);
	REQUIRE(rough.size() == 5);
	// Where plain quantizing to 1/8 would put the slices: their snap points, the hitpoints, on the grid.
	const std::vector<int64_t> plain{
		0, (g_quarter / 2) - 240, g_quarter - 240, (2 * g_quarter) - 240, (3 * g_quarter) - 240};
	for (size_t i = 0; i < rough.size(); ++i)
	{
		CAPTURE(i);
		CHECK(std::abs(rough[i] - plain[i]) <= 750);
	}
	// The randomizing moved them off the grid, and on both tracks alike: it is seeded by position.
	CHECK(rough != plain);
	CHECK(startsOn(client, room.track) == rough);

	// Soft quantizing goes on from there, halfway to the grid, on both tracks alike.
	run(client, "quantize.apply",
		{{"events", sliced.at("events")}, {"settings", {{"soft", true}, {"strength", 0.5}, {"randomize", 0}}}});
	const std::vector<int64_t> soft = startsOn(client, drums.track);
	CHECK(startsOn(client, room.track) == soft);
	REQUIRE(soft.size() == rough.size());
	for (size_t i = 0; i < soft.size(); ++i)
	{
		CAPTURE(i);
		CHECK(std::abs(soft[i] - ((rough[i] + plain[i]) / 2)) <= 2);
	}
}

TEST_CASE("Crossfade leaves a gap that the later event's audio can't close", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// An event at 2000 with a hitpoint 2000 frames in, sliced there and quantized to 1/16: the first slice moves to
	// 0 and ends at 2000, the second one to 6000, with only 2000 frames of audio before it.
	const Loop loop = addLoop(client, dir, "loop", clicks({2000}, 12000), 2000);
	setHitpoints(client, loop.clip, {2000});
	const Json sliced = run(client, "quantize.slice",
		{{"events", {loop.event}}, {"priorities", {{{"track", loop.track}, {"priority", 1}}}}});
	run(client, "quantize.set", {{"preset", "1/16"}});
	run(client, "quantize.apply", {{"events", sliced.at("events")}});
	REQUIRE(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{0, 0, 2000}, {6000, 2000, 10000}});

	// The gap closes down to 4000, and the first slice, 10 ms longer, still ends before it: no crossfade there.
	run(client, "quantize.crossfade", {{"events", sliced.at("events")}});
	CHECK(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{0, 0, 2000}, {4000, 0, 12000}});
	const Json project = run(client, "project.get");
	for (const Json& event : project.at("audioEvents"))
		CHECK(!event.at("crossfade").is_object());
}

TEST_CASE("Reset Quantize undoes what the panel's Crossfade step changed", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Sliced, quantized to 1/8 and crossfaded: two slices are stretched and moved where a gap closed and cut short
	// again where they then overlapped the next one, and every slice but the first crossfades into the one before it.
	// Resetting puts each back to its position, offset and length and removes the crossfades, so the slices tile
	// again: Reset Quantize also resets the length changes made in the quantize panel (reference manual pp. 367, 379).
	const std::vector<int64_t> played{6200, 25600, 47300, 72800};
	const Loop loop = addLoop(client, dir, "loop", clicks(played, 4 * g_quarter));
	setHitpoints(client, loop.clip, played);
	const Json sliced = run(client, "quantize.slice", {{"events", {loop.event}}, {"offset", 5.0}});
	const std::vector<std::vector<int64_t>> before = eventsOn(client, loop.track);
	REQUIRE(before ==
		std::vector<std::vector<int64_t>>{
			{0, 0, 5960}, {5960, 5960, 19400}, {25360, 25360, 21700}, {47060, 47060, 25500}, {72560, 72560, 23440}});
	run(client, "quantize.set", {{"preset", "1/8"}});
	run(client, "quantize.apply", {{"events", sliced.at("events")}});
	run(client, "quantize.crossfade", {{"events", sliced.at("events")}, {"length", 2.0}});
	REQUIRE(eventsOn(client, loop.track) ==
		std::vector<std::vector<int64_t>>{
			{0, 0, 6056}, {5960, 160, 17896}, {23760, 25360, 21796}, {45460, 44760, 26396}, {71760, 72560, 23440}});
	REQUIRE(crossfades(client) == 4);

	SECTION("reset puts every slice back")
	{
		run(client, "quantize.reset", {{"events", sliced.at("events")}});
		CHECK(lastStep(client) == "Reset quantize");
		CHECK(eventsOn(client, loop.track) == before);
		CHECK(crossfades(client) == 0);
	}

	SECTION("a project saved after the crossfade keeps it")
	{
		const std::string file = (dir.path() / "crossfaded.dawproject").u8string();
		run(client, "project.save", {{"path", file}, {"embedMedia", false}});
		run(client, "project.new");
		run(client, "project.load", {{"path", file}});
		REQUIRE(eventsOn(client, loop.track).size() == 5);
		run(client, "quantize.reset", {{"events", sliced.at("events")}});
		CHECK(eventsOn(client, loop.track) == before);
		CHECK(crossfades(client) == 0);
	}

	SECTION("splitting a slice whose start the Crossfade step stretched keeps both pieces resettable")
	{
		// The step stretched the second slice's start: it plays from 160 instead of 5960 (#1333). The piece after a
		// split at 10000 gets its original offset and length from the recorded original's, cut where the piece
		// before it ends, so resetting plays the right clip section and the pieces tile the original slice again.
		const ObjectId second = sliced.at("events").at(1).get<ObjectId>();
		REQUIRE(eventsOn(client, loop.track).at(1) == std::vector<int64_t>{5960, 160, 17896});
		const Json pieces = run(client, "event.split", {{"events", {second}}, {"positions", {10000}}}).at("events");
		REQUIRE(pieces.size() == 1);
		const auto right = pieces.at(0).get<ObjectId>();
		CHECK(byId(client, "audioEvents", right).at("quantizeOrigin") ==
			Json({{"original", originState(10000, 10000, 15360)}, {"quantized", originState(10000, 4200, 13856)}}));

		Json events = sliced.at("events");
		events.push_back(right);
		run(client, "quantize.reset", {{"events", events}});
		CHECK(eventsOn(client, loop.track) ==
			std::vector<std::vector<int64_t>>{{0, 0, 5960}, {5960, 5960, 4040}, {10000, 10000, 15360},
				{25360, 25360, 21700}, {47060, 47060, 25500}, {72560, 72560, 23440}});
		CHECK(crossfades(client) == 0);
	}

	SECTION("resetting removes a piece split off inside the part the Crossfade step added")
	{
		// The step grew the first slice by the crossfade into the next one: it is {0, 0, 6056}, its original was
		// {0, 0, 5960}. A split at 6000 falls inside the added part, past the end of the original's content, so
		// there is nothing of the original left for the piece after it: its original has no length. Resetting
		// returns to the original, unquantized state and resets the panel's length changes (reference manual
		// p. 367), so that piece goes, in the same undo step, and the piece before it is the whole original slice
		// again (#1435).
		const ObjectId firstSlice = sliced.at("events").at(0).get<ObjectId>();
		REQUIRE(eventsOn(client, loop.track).at(0) == std::vector<int64_t>{0, 0, 6056});
		const Json pieces = run(client, "event.split", {{"events", {firstSlice}}, {"positions", {6000}}}).at("events");
		REQUIRE(pieces.size() == 1);
		const auto right = pieces.at(0).get<ObjectId>();
		CHECK(byId(client, "audioEvents", right).at("quantizeOrigin") ==
			Json({{"original", originState(6000, 6000, 0)}, {"quantized", originState(6000, 6000, 56)}}));
		const std::vector<std::vector<int64_t>> split = eventsOn(client, loop.track);

		Json events = sliced.at("events");
		events.push_back(right);
		const Json reset = run(client, "quantize.reset", {{"events", events}});
		CHECK(reset.at("removed") == Json::array({right}));
		CHECK(eventsOn(client, loop.track) == before);
		CHECK(crossfades(client) == 0);

		run(client, "history.undo");
		CHECK(eventsOn(client, loop.track) == split);
		CHECK(crossfades(client) == 4);
	}

	SECTION("a locked piece split off inside the added part stays")
	{
		const ObjectId firstSlice = sliced.at("events").at(0).get<ObjectId>();
		const auto right = run(client, "event.split", {{"events", {firstSlice}}, {"positions", {6000}}})
							   .at("events")
							   .at(0)
							   .get<ObjectId>();
		run(client, "event.set", {{"events", {right}}, {"lock", true}});
		Json events = sliced.at("events");
		events.push_back(right);
		const Json reset = run(client, "quantize.reset", {{"events", events}});
		CHECK(reset.at("removed").empty());
		CHECK(eventsOn(client, loop.track) ==
			std::vector<std::vector<int64_t>>{{0, 0, 5960}, {5960, 5960, 19400}, {6000, 6000, 56},
				{25360, 25360, 21700}, {47060, 47060, 25500}, {72560, 72560, 23440}});
	}

	SECTION("a crossfade into the next slice follows a split at the original's end, which resetting removes")
	{
		// A split at 5960 falls exactly at the end of the first slice's original: the piece after it has nothing
		// of the original, but the second slice starts there, so its crossfade from the first follows the split
		// onto that piece all the same (#1333). Resetting removes the piece and takes out the crossfade the panel
		// made (#1435).
		const ObjectId firstSlice = sliced.at("events").at(0).get<ObjectId>();
		const Json pieces = run(client, "event.split", {{"events", {firstSlice}}, {"positions", {5960}}}).at("events");
		REQUIRE(pieces.size() == 1);
		const auto right = pieces.at(0).get<ObjectId>();
		REQUIRE(crossfadeOf(client, sliced.at("events").at(1).get<ObjectId>()).at("from") == right);

		Json events = sliced.at("events");
		events.push_back(right);
		const Json reset = run(client, "quantize.reset", {{"events", events}});
		CHECK(reset.at("removed") == Json::array({right}));
		CHECK(eventsOn(client, loop.track) == before);
		CHECK(crossfades(client) == 0);
	}

	SECTION("splitting a slice inside the stretch at its start keeps both pieces resettable")
	{
		// The mirror of the added part at the end: the step stretched the second slice's start by 5800 frames to
		// close the gap. A split at 8960 falls inside the stretch; the piece after it plays from 3160 now but from
		// 8960 in the original, and resetting gives both pieces their original offset and length back, so that
		// they tile the original slice again (#1333).
		const ObjectId second = sliced.at("events").at(1).get<ObjectId>();
		const Json pieces = run(client, "event.split", {{"events", {second}}, {"positions", {8960}}}).at("events");
		REQUIRE(pieces.size() == 1);
		const auto right = pieces.at(0).get<ObjectId>();
		CHECK(byId(client, "audioEvents", right).at("quantizeOrigin") ==
			Json({{"original", originState(8960, 8960, 16400)}, {"quantized", originState(8960, 3160, 14896)}}));

		Json events = sliced.at("events");
		events.push_back(right);
		run(client, "quantize.reset", {{"events", events}});
		CHECK(eventsOn(client, loop.track) ==
			std::vector<std::vector<int64_t>>{{0, 0, 5960}, {5960, 5960, 3000}, {8960, 8960, 16400},
				{25360, 25360, 21700}, {47060, 47060, 25500}, {72560, 72560, 23440}});
		CHECK(crossfades(client) == 0);
	}

	SECTION("splitting the tail of a slice the step stretched at its start and made longer than its original")
	{
		// The fourth slice plays from 44760 instead of 47060 and is 26396 frames long instead of 25500. A split
		// 25600 frames in falls past its original's length, so the piece after it gets no length of the original,
		// although it plays frames of the original slice: the piece before it takes the whole original back.
		const ObjectId fourth = sliced.at("events").at(3).get<ObjectId>();
		REQUIRE(eventsOn(client, loop.track).at(3) == std::vector<int64_t>{45460, 44760, 26396});
		const Json pieces = run(client, "event.split", {{"events", {fourth}}, {"positions", {71060}}}).at("events");
		REQUIRE(pieces.size() == 1);
		const auto right = pieces.at(0).get<ObjectId>();
		CHECK(byId(client, "audioEvents", right).at("quantizeOrigin").at("original").at("length") == 0);

		Json events = sliced.at("events");
		events.push_back(right);
		const Json reset = run(client, "quantize.reset", {{"events", events}});
		CHECK(reset.at("removed") == Json::array({right}));
		CHECK(eventsOn(client, loop.track) == before);
		CHECK(crossfades(client) == 0);
	}

	SECTION("reset keeps an end trimmed by hand on a slice the step stretched")
	{
		// Each part of the origin is judged by itself: the stretched offset goes back, the hand-trimmed length
		// stays, so the slice's end moves in the clip by the stretch.
		const ObjectId second = sliced.at("events").at(1).get<ObjectId>();
		run(client, "event.resize", {{"events", {second}}, {"edge", "end"}, {"delta", -1000}});
		REQUIRE(eventsOn(client, loop.track).at(1) == std::vector<int64_t>{5960, 160, 16896});
		run(client, "quantize.reset", {{"events", sliced.at("events")}});
		CHECK(eventsOn(client, loop.track).at(1) == std::vector<int64_t>{5960, 5960, 16896});
	}
}

TEST_CASE("Reset Quantize keeps a crossfade the event had before quantizing", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Three overlapping events of one clip, the first crossfading into the second by hand before quantizing.
	// Quantizing moves them, and the panel's Crossfade step cuts and stretches them and crossfades the second into
	// the third. Resetting returns to the state before quantizing (reference manual p. 367), which that crossfade is
	// part of: the one made by hand comes back and the one the step made goes.
	const Track loop = addTrack(client, dir, "loop", clicks({1000}, 4 * g_quarter));
	const ObjectId track = loop.track;
	const ObjectId first = loop.add(client, 1000, 0, 30000);
	const ObjectId second = loop.add(client, 25000, 24000, 30000);
	const ObjectId third = loop.add(client, 49000, 48000, 30000);
	run(client, "crossfade.set", {{"from", first}, {"to", second}, {"mode", "equalPower"}});
	REQUIRE(crossfadeOf(client, second).at("mode") == "equalPower");
	const std::vector<std::vector<int64_t>> before = eventsOn(client, track);
	REQUIRE(
		before == std::vector<std::vector<int64_t>>{{1000, 0, 30000}, {25000, 24000, 30000}, {49000, 48000, 30000}});

	run(client, "quantize.set", {{"preset", "1/4"}});
	const Json events = Json::array({first, second, third});
	run(client, "quantize.apply", {{"events", events}});
	run(client, "quantize.crossfade", {{"events", events}});
	REQUIRE(crossfadeOf(client, second).at("mode") == "equalPower");
	REQUIRE(crossfadeOf(client, third).at("from") == second);

	SECTION("reset puts the hand-made crossfade back and removes the one the step made")
	{
		run(client, "quantize.reset", {{"events", events}});
		CHECK(eventsOn(client, track) == before);
		const Json kept = crossfadeOf(client, second);
		REQUIRE(kept.is_object());
		CHECK(kept.at("from") == first);
		CHECK(kept.at("mode") == "equalPower");
		CHECK(!crossfadeOf(client, third).is_object());
	}

	SECTION("a project saved after the crossfade keeps it")
	{
		const std::string saved = (dir.path() / "crossfaded.dawproject").u8string();
		run(client, "project.save", {{"path", saved}, {"embedMedia", false}});
		run(client, "project.new");
		run(client, "project.load", {{"path", saved}});
		REQUIRE(eventsOn(client, track).size() == 3);
		run(client, "quantize.reset", {{"events", events}});
		CHECK(eventsOn(client, track) == before);
		CHECK(crossfadeOf(client, second).at("mode") == "equalPower");
		CHECK(!crossfadeOf(client, third).is_object());
	}

	SECTION("splitting the event the hand-made crossfade comes from")
	{
		// The crossfade follows the piece after the split, and its origin's crossfades follow with it (#1333), so
		// that resetting keeps the hand-made one, from the piece that is then the neighbour before the event.
		const Json pieces = run(client, "event.split", {{"events", {first}}, {"positions", {20000}}}).at("events");
		REQUIRE(pieces.size() == 1);
		const auto right = pieces.at(0).get<ObjectId>();
		REQUIRE(crossfadeOf(client, second).at("from") == right);
		Json all = events;
		all.push_back(right);
		run(client, "quantize.reset", {{"events", all}});
		CHECK(eventsOn(client, track) ==
			std::vector<std::vector<int64_t>>{
				{1000, 0, 20000}, {21000, 20000, 10000}, {25000, 24000, 30000}, {49000, 48000, 30000}});
		const Json kept = crossfadeOf(client, second);
		REQUIRE(kept.is_object());
		CHECK(kept.at("from") == right);
		CHECK(kept.at("mode") == "equalPower");
		CHECK(!crossfadeOf(client, third).is_object());
	}
}

TEST_CASE(
	"Reset Quantize removes a piece split off past the original and keeps the crossfade before it", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Two overlapping events, the first crossfading into the second by hand before quantizing. Quantizing opens a
	// gap between them, which the panel's Crossfade step closes by stretching the second's start back to the
	// first's end, and then the first grows into the second by the crossfade. A split at the end of the first's
	// original leaves a piece with nothing of the original, which the second's crossfade now comes from. Resetting
	// removes that piece (#1435) and the hand-made crossfade comes back from the piece before it, which is the
	// whole original again (reference manual p. 367).
	const Track loop = addTrack(client, dir, "loop", clicks({1000}, 4 * g_quarter));
	// The piece before is the event that ended where the piece starts, not the first in id order of those that
	// play its clip frames (#1566): the second event, stretched back over them, and another copy of the clip
	// elsewhere on the track play them too.
	enum class Created : uint8_t
	{
		FirstFirst,
		SecondFirst,
		CopyFirst,
	};
	const Created created = GENERATE(Created::FirstFirst, Created::SecondFirst, Created::CopyFirst);
	CAPTURE(created);
	if (created == Created::CopyFirst)
		loop.add(client, 200000, 0, 20000);
	ObjectId second = created == Created::SecondFirst ? loop.add(client, 19000, 18000, 30000) : 0;
	const ObjectId first = loop.add(client, 1000, 0, 20000);
	if (second == 0)
		second = loop.add(client, 19000, 18000, 30000);
	run(client, "crossfade.set", {{"from", first}, {"to", second}, {"mode", "equalPower"}});
	const std::vector<std::vector<int64_t>> before = eventsOn(client, loop.track);
	run(client, "quantize.set", {{"preset", "1/4"}});
	const Json events = Json::array({first, second});
	run(client, "quantize.apply", {{"events", events}});
	run(client, "quantize.crossfade", {{"events", events}});
	REQUIRE(eventsOn(client, loop.track).at(1) == std::vector<int64_t>{20000, 14000, 34000});
	REQUIRE(eventsOn(client, loop.track).at(0).at(2) > 20000);

	const Json pieces = run(client, "event.split", {{"events", {first}}, {"positions", {20000}}}).at("events");
	REQUIRE(pieces.size() == 1);
	const auto right = pieces.at(0).get<ObjectId>();
	REQUIRE(crossfadeOf(client, second).at("from") == right);
	const std::vector<std::vector<int64_t>> split = eventsOn(client, loop.track);
	Json all = events;
	all.push_back(right);
	// The events are back as before quantizing, with the hand-made crossfade.
	const auto checkBack = [&]()
	{
		CHECK(eventsOn(client, loop.track) == before);
		const Json kept = crossfadeOf(client, second);
		REQUIRE(kept.is_object());
		CHECK(kept.at("from") == first);
		CHECK(kept.at("mode") == "equalPower");
	};
	const auto checkReset = [&](const Json& reset, const Json& removed)
	{
		CHECK(reset.at("removed") == removed);
		checkBack();
	};

	SECTION("resetting all of them")
	{
		checkReset(run(client, "quantize.reset", {{"events", all}}), Json::array({right}));
	}

	SECTION("redoing the reset after undoing it")
	{
		run(client, "quantize.reset", {{"events", all}});
		run(client, "history.undo");
		CHECK(eventsOn(client, loop.track) == split);
		CHECK(crossfadeOf(client, second).at("from") == right);
		run(client, "history.redo");
		checkBack();
	}

	SECTION("resetting the split event and its piece first and the neighbour second")
	{
		// Removing the piece moves the crossfade from it to the piece before it, the whole original again (#1566).
		// The neighbour's origin keeps in step, so that resetting it later still takes the crossfade as the panel
		// left it and puts back the one from before quantizing.
		const Json reset = run(client, "quantize.reset", {{"events", {first, right}}});
		CHECK(reset.at("removed") == Json::array({right}));
		CHECK(crossfadeOf(client, second).at("from") == first);
		checkReset(run(client, "quantize.reset", {{"events", {second}}}), Json::array());
	}

	SECTION("a locked neighbour keeps its crossfade, from the piece before the removed one")
	{
		// A locked event stays as it is when resetting (reference manual p. 276), so its crossfade from the piece
		// goes to the piece before it rather than ending with the piece (#1566). Unlocked and reset, the neighbour
		// gets the one from before quantizing back.
		const auto timing = [&](ObjectId id)
		{
			const Json event = byId(client, "audioEvents", id);
			return std::vector<int64_t>{event.at("position").get<int64_t>(), event.at("offset").get<int64_t>(),
				event.at("length").get<int64_t>()};
		};
		const std::vector<int64_t> locked = timing(second);
		run(client, "event.set", {{"events", {second}}, {"lock", true}});
		const Json reset = run(client, "quantize.reset", {{"events", all}});
		CHECK(reset.at("removed") == Json::array({right}));
		CHECK(timing(second) == locked);
		CHECK(crossfadeOf(client, second).at("from") == first);
		run(client, "event.set", {{"events", {second}}, {"lock", false}});
		checkReset(run(client, "quantize.reset", {{"events", {second}}}), Json::array());
	}

	SECTION("an original crossfade from an event removed by hand doesn't come back")
	{
		// The neighbour's original crossfade still names the split event, while the one it has now comes from the
		// piece. With that event removed by hand, resetting the neighbour has nothing for it to come from (#1566).
		run(client, "event.remove", {{"events", {first}}});
		run(client, "quantize.reset", {{"events", {second}}});
		CHECK(!crossfadeOf(client, second).is_object());
		CHECK(byId(client, "audioEvents", second).at("position") == before.at(1).at(0));
	}

	SECTION("without the event the piece was split from, the neighbour's crossfade ends")
	{
		// Only an event that ended where the piece starts is the piece before it (#1642): not the copy of the loop
		// elsewhere on the track, and not the neighbour, which plays the piece's clip frames too.
		run(client, "event.remove", {{"events", {first}}});
		const Json reset = run(client, "quantize.reset", {{"events", {right}}});
		CHECK(reset.at("removed") == Json::array({right}));
		CHECK(!crossfadeOf(client, second).is_object());
	}

	SECTION("a project saved with the piece")
	{
		// The piece's original has a length of 0 in the file.
		const std::string file = (dir.path() / "piece.dawproject").u8string();
		run(client, "project.save", {{"path", file}, {"embedMedia", false}});
		run(client, "project.new");
		run(client, "project.load", {{"path", file}});
		REQUIRE(byId(client, "audioEvents", right).at("quantizeOrigin").at("original").at("length") == 0);
		checkReset(run(client, "quantize.reset", {{"events", all}}), Json::array({right}));
	}

	SECTION("the events of an audio part")
	{
		const Json part = run(client, "event.toPart", {{"events", all}}).at("events").at(0);
		checkReset(run(client, "quantize.reset", {{"audioParts", {part}}}), Json::array({right}));
	}

	SECTION("splitting the piece again leaves two pieces with nothing of the original")
	{
		const Json halves = run(client, "event.split", {{"events", {right}}, {"positions", {20100}}}).at("events");
		REQUIRE(halves.size() == 1);
		const auto half = halves.at(0).get<ObjectId>();
		CHECK(byId(client, "audioEvents", half).at("quantizeOrigin").at("original").at("length") == 0);
		all.push_back(half);
		checkReset(run(client, "quantize.reset", {{"events", all}}), Json::array({right, half}));
	}

	SECTION("a piece whose end was trimmed by hand stays, past the end of the original")
	{
		// Its hand-set length counts as its original, as any length set by other means does: it goes back by the
		// distance its event moved, from 20000 to 21000, and keeps that length.
		const int64_t length = byId(client, "audioEvents", right).at("length").get<int64_t>();
		REQUIRE(length > 100);
		run(client, "event.resize", {{"events", {right}}, {"edge", "end"}, {"delta", -100}});
		const Json reset = run(client, "quantize.reset", {{"events", all}});
		CHECK(reset.at("removed").empty());
		std::vector<std::vector<int64_t>> expected = before;
		expected.push_back({21000, 20000, length - 100});
		std::sort(expected.begin(), expected.end());
		CHECK(eventsOn(client, loop.track) == expected);
		CHECK(crossfadeOf(client, second).at("from") == first);
	}
}

TEST_CASE(
	"Reset Quantize moves a crossfade changed by hand from a piece it removes to the piece before", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// As in the test before, but with the crossfade nudged right, so that the second event starts after the end of
	// the first's original: the piece split off there overlaps its start, and the crossfade from the piece can be
	// changed by hand. Changed by other means since quantizing, it counts as the second's original and stays when
	// resetting; when the piece it comes from goes, it comes from the piece before, the whole original that
	// overlaps the second again, rather than ending with the piece (#1566).
	const Track loop = addTrack(client, dir, "loop", clicks({1000}, 4 * g_quarter));
	const ObjectId first = loop.add(client, 1000, 0, 20000);
	const ObjectId second = loop.add(client, 19000, 18000, 30000);
	run(client, "crossfade.set", {{"from", first}, {"to", second}, {"mode", "equalPower"}});
	const std::vector<std::vector<int64_t>> before = eventsOn(client, loop.track);
	run(client, "quantize.set", {{"preset", "1/4"}});
	const Json events = Json::array({first, second});
	run(client, "quantize.apply", {{"events", events}});
	run(client, "quantize.crossfade", {{"events", events}});
	run(client, "quantize.nudgeCrossfade", {{"events", {second}}});
	REQUIRE(eventsOn(client, loop.track).at(1).at(0) == 20048);
	const auto right =
		run(client, "event.split", {{"events", {first}}, {"positions", {20000}}}).at("events").at(0).get<ObjectId>();
	REQUIRE(crossfadeOf(client, second).at("from") == right);
	run(client, "crossfade.set", {{"from", right}, {"to", second}, {"mode", "equalGain"}});
	const auto checkHandMade = [&]()
	{
		CHECK(eventsOn(client, loop.track) == before);
		const Json kept = crossfadeOf(client, second);
		REQUIRE(kept.is_object());
		CHECK(kept.at("from") == first);
		CHECK(kept.at("mode") == "equalGain");
	};

	SECTION("all at once")
	{
		CHECK(
			run(client, "quantize.reset", {{"events", {first, second, right}}}).at("removed") == Json::array({right}));
		checkHandMade();
	}
	SECTION("the neighbour after the others")
	{
		CHECK(run(client, "quantize.reset", {{"events", {first, right}}}).at("removed") == Json::array({right}));
		CHECK(crossfadeOf(client, second).at("from") == first);
		run(client, "quantize.reset", {{"events", {second}}});
		checkHandMade();
	}
}

TEST_CASE(
	"Reset Quantize moves a crossfade from a piece it removes to the event crossfading into the piece", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// As in the test before, with the crossfade nudged right a few times, so that the piece split off at the end of
	// the first's original is longer than a crossfade. The Crossfade step on the first and the piece makes the first
	// grow into the piece: it ends past the piece's start by the crossfade's length, and the piece's crossfade comes
	// from it. Removing the piece, the second's crossfade from it goes to the first, the whole original again, rather
	// than ending with the piece (#1723).
	const Track loop = addTrack(client, dir, "loop", clicks({1000}, 4 * g_quarter));
	// Another event of the clip, with a lower id than the first, out of the way until a section places it.
	const ObjectId copy = loop.add(client, 90000, 0, 1000);
	const ObjectId first = loop.add(client, 1000, 0, 20000);
	const ObjectId second = loop.add(client, 19000, 18000, 30000);
	run(client, "crossfade.set", {{"from", first}, {"to", second}, {"mode", "equalPower"}});
	run(client, "quantize.set", {{"preset", "1/4"}});
	const Json events = Json::array({first, second});
	run(client, "quantize.apply", {{"events", events}});
	run(client, "quantize.crossfade", {{"events", events}});
	run(client, "quantize.nudgeCrossfade", {{"events", {second}}, {"amount", 4.0}});
	const auto right =
		run(client, "event.split", {{"events", {first}}, {"positions", {20000}}}).at("events").at(0).get<ObjectId>();
	REQUIRE(crossfadeOf(client, second).at("from") == right);
	run(client, "quantize.crossfade", {{"events", {first, right}}});
	REQUIRE(crossfadeOf(client, right).at("from") == first);
	REQUIRE(byId(client, "audioEvents", first).at("length").get<int64_t>() > 20001);
	const auto checkFromFirst = [&]()
	{
		CHECK(run(client, "quantize.reset", {{"events", {first, right}}}).at("removed") == Json::array({right}));
		CHECK(eventsOn(client, loop.track).at(0) == std::vector<int64_t>{1000, 0, 20000});
		const Json kept = crossfadeOf(client, second);
		REQUIRE(kept.is_object());
		CHECK(kept.at("from") == first);
	};

	SECTION("the first is the piece before")
	{
		checkFromFirst();
	}

	SECTION("also over an event that ends where the piece starts and plays its clip frames up to there (#1805)")
	{
		// Such as an overlapping copy, with a lower id: it ends exactly where the piece starts, but the first, which
		// the piece crossfaded from, wins.
		const Json piece = byId(client, "audioEvents", right);
		const int64_t start = piece.at("position").get<int64_t>();
		const int64_t offset = piece.at("offset").get<int64_t>();
		REQUIRE(start > 1000);
		REQUIRE(offset > 1000);
		run(client, "audioEvent.set",
			{{"event", copy}, {"position", start - 1000}, {"offset", offset - 1000}, {"length", 1000}});
		checkFromFirst();
	}

	SECTION("not once the first was moved away from the piece by hand (#1805)")
	{
		// The piece still crossfades from it, but a bar later it no longer overlaps the piece: the crossfade ends, as
		// without an event before.
		run(client, "event.move", {{"events", {first}}, {"delta", 4 * g_quarter}});
		REQUIRE(crossfadeOf(client, right).at("from") == first);
		CHECK(run(client, "quantize.reset", {{"events", {right}}}).at("removed") == Json::array({right}));
		CHECK(!crossfadeOf(client, second).is_object());
	}
}

TEST_CASE("Reset Quantize leaves locked audio events as they are", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// A locked event is protected from being moved and edited (reference manual p. 276), so resetting leaves it
	// where quantizing left it, as quantizing leaves it where it is.
	run(client, "quantize.set", {{"preset", "1/4"}});
	const Loop loop = addLoop(client, dir, "loop", clicks({1000}, 4 * g_quarter), 2000);
	run(client, "quantize.apply", {{"events", {loop.event}}});
	run(client, "event.set", {{"events", {loop.event}}, {"lock", true}});
	const Json reset = run(client, "quantize.reset", {{"events", {loop.event}}});
	CHECK(reset.at("events").empty());
	CHECK(positionOf(client, loop.event) == 0);

	// Unlocked, it goes back.
	run(client, "event.set", {{"events", {loop.event}}, {"lock", false}});
	run(client, "quantize.reset", {{"events", {loop.event}}});
	CHECK(positionOf(client, loop.event) == 2000);
}

TEST_CASE("Reset Quantize keeps an end trimmed by hand after quantizing", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	run(client, "quantize.set", {{"preset", "1/4"}});
	const Loop loop = addLoop(client, dir, "loop", clicks({1000}, 4 * g_quarter), 2000);
	run(client, "quantize.apply", {{"events", {loop.event}}});
	REQUIRE(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{0, 0, 4 * g_quarter}});
	// Trimming the end by hand is a length change of the user's own, not one the quantize panel made, so resetting
	// leaves it, as a note's length changed by other means stays.
	run(client, "event.resize", {{"events", {loop.event}}, {"edge", "end"}, {"delta", -30000}});
	REQUIRE(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{0, 0, 66000}});

	run(client, "quantize.reset", {{"events", {loop.event}}});
	CHECK(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{2000, 0, 66000}});

	// Quantizing again goes on from the trimmed length too, and keeps the original position.
	run(client, "quantize.apply", {{"events", {loop.event}}});
	CHECK(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{0, 0, 66000}});
	run(client, "quantize.reset", {{"events", {loop.event}}});
	CHECK(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{2000, 0, 66000}});
}

TEST_CASE("Reset Quantize keeps a crossfade made by hand after quantizing", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Two overlapping events of one clip, quantized and then crossfaded by hand: that crossfade is a change of the
	// user's own, not one the quantize panel made, so resetting leaves it.
	const Track loop = addTrack(client, dir, "loop", clicks({1000}, 4 * g_quarter));
	const ObjectId first = loop.add(client, 1000, 0, 30000);
	const ObjectId second = loop.add(client, 25000, 24000, 30000);
	run(client, "quantize.set", {{"preset", "1/4"}});
	const Json events = Json::array({first, second});
	run(client, "quantize.apply", {{"events", events}});
	REQUIRE(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{0, 0, 30000}, {24000, 24000, 30000}});
	run(client, "crossfade.set", {{"from", first}, {"to", second}, {"mode", "equalPower"}});
	REQUIRE(crossfadeOf(client, second).at("mode") == "equalPower");

	run(client, "quantize.reset", {{"events", events}});
	CHECK(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{1000, 0, 30000}, {25000, 24000, 30000}});
	const Json kept = crossfadeOf(client, second);
	REQUIRE(kept.is_object());
	CHECK(kept.at("from") == first);
	CHECK(kept.at("mode") == "equalPower");
}

TEST_CASE("a project file from before the origin kept the offset and length still loads", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	run(client, "quantize.set", {{"preset", "1/4"}});
	const Loop loop = addLoop(client, dir, "loop", clicks({1000}, 4 * g_quarter), 2000);
	run(client, "quantize.apply", {{"events", {loop.event}}});
	REQUIRE(positionOf(client, loop.event) == 0);
	const std::string file = (dir.path() / "quantized.dawproject").u8string();
	run(client, "project.save", {{"path", file}, {"embedMedia", false}});

	// An origin as GNUbase wrote it before it kept the offset and length too: a bare position for each state.
	std::map<std::string, std::string> entries = test::readZip(file);
	Json extension = Json::parse(entries.at("gnubase.json"));
	size_t origins = 0;
	for (Json& event : extension.at("project").at("audioEvents"))
		if (const auto origin = event.find("quantizeOrigin"); origin != event.end())
		{
			*origin = {{"original", origin->at("original").at("position")},
				{"quantized", origin->at("quantized").at("position")}};
			++origins;
		}
	REQUIRE(origins == 1);
	entries["gnubase.json"] = extension.dump();
	test::writeZip(file, entries);

	run(client, "project.new");
	run(client, "project.load", {{"path", file}});
	// Resetting puts the position back; the offset and length, which the file doesn't have, stay.
	run(client, "quantize.reset", {{"events", {loop.event}}});
	CHECK(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{2000, 0, 4 * g_quarter}});
}

TEST_CASE("Hitpoints of one track at the same place give one position", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Two events of one track playing the same clip at the same place: their hitpoints coincide. The range merges
	// hitpoints of different tracks only, so without dropping equal neighbours each position would come twice.
	const std::vector<int64_t> played{25600, 47300};
	const Loop loop = addLoop(client, dir, "loop", clicks(played, 4 * g_quarter));
	setHitpoints(client, loop.clip, played);
	const ObjectId again = run(client, "audioEvent.add",
		{{"track", loop.track}, {"clip", loop.clip}, {"position", 0}, {"offset", 0},
			{"length", static_cast<int64_t>(4 * g_quarter)}})
							   .at("event")
							   .at("id")
							   .get<ObjectId>();
	const Json events = Json::array({loop.event, again});
	// The markers first: slicing cuts the events at the positions, so their hitpoints stop being inside them.
	CHECK(
		run(client, "quantize.createWarpMarkers", {{"events", events}}).at("positions") == Json::array({25600, 47300}));
	CHECK(run(client, "quantize.slice", {{"events", events}}).at("positions") == Json::array({25600, 47300}));
}

TEST_CASE("warp markers for several audio tracks come from the hitpoints of the highest priority", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const Loop a = addLoop(client, dir, "a", clicks({1000, 25600}, 2 * g_quarter));
	const Loop b = addLoop(client, dir, "b", clicks({1000, 25600}, 2 * g_quarter));
	setHitpoints(client, a.clip, {1000, 25600});
	setHitpoints(client, b.clip, {1200, 25500, 40000});
	// The same priority: the first hitpoint within the range counts (reference manual p. 380).
	const Json result = run(client, "quantize.createWarpMarkers", {{"events", {a.event, b.event}}, {"range", 10.0}});
	CHECK(lastStep(client) == "Create warp markers");
	CHECK(result.at("positions") == Json::array({1000, 25500, 40000}));
	const Json expected = Json::array({{{"position", 1000}, {"target", 1000}}, {{"position", 25500}, {"target", 25500}},
		{{"position", 40000}, {"target", 40000}}});
	CHECK(markersOf(client, a.clip) == expected);
	CHECK(markersOf(client, b.clip) == expected);
	// Warp quantized, both tracks' markers go to the grid.
	run(client, "quantize.set", {{"preset", "1/4"}, {"settings", {{"warpQuantize", true}}}});
	run(client, "quantize.apply", {{"events", {a.event, b.event}}});
	CHECK(markersOf(client, a.clip) == markersOf(client, b.clip));
	CHECK(markersOf(client, a.clip).at(1).at("target") == g_quarter);
}

TEST_CASE("Nudge Crossfade moves the crossfades of quantized slices by a millisecond", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Sliced, quantized and crossfaded as in the test of Reset Quantize after the Crossfade step: the first slice goes
	// 96 samples (2 ms) into the second one. Nudging moves that area by 48 samples (1 ms) without changing its length,
	// so it can go off an attack the Offset didn't keep clear (reference manual p. 379). Every crossfade moves, so a
	// slice between two keeps its length: both its start and its end move.
	const std::vector<int64_t> played{6200, 25600, 47300, 72800};
	const Loop loop = addLoop(client, dir, "loop", clicks(played, 4 * g_quarter));
	setHitpoints(client, loop.clip, played);
	const Json sliced = run(client, "quantize.slice", {{"events", {loop.event}}, {"offset", 5.0}});
	const Json& events = sliced.at("events");
	const std::vector<std::vector<int64_t>> before = eventsOn(client, loop.track);
	run(client, "quantize.set", {{"preset", "1/8"}});
	run(client, "quantize.apply", {{"events", events}});
	run(client, "quantize.crossfade", {{"events", events}, {"length", 2.0}});
	const std::vector<std::vector<int64_t>> crossfaded = eventsOn(client, loop.track);
	REQUIRE(crossfaded[0] == std::vector<int64_t>{0, 0, 6056});
	REQUIRE(crossfaded[1] == std::vector<int64_t>{5960, 160, 17896});

	SECTION("right")
	{
		run(client, "quantize.nudgeCrossfade", {{"events", events}});
		CHECK(lastStep(client) == "Nudge crossfade");
		const std::vector<std::vector<int64_t>> nudged = eventsOn(client, loop.track);
		CHECK(nudged[0] == std::vector<int64_t>{0, 0, 6104});
		CHECK(nudged[1] == std::vector<int64_t>{6008, 208, 17896});
		CHECK(crossfades(client) == 4);
		SECTION("and reset puts every slice back")
		{
			run(client, "quantize.reset", {{"events", events}});
			CHECK(eventsOn(client, loop.track) == before);
			CHECK(crossfades(client) == 0);
		}
		SECTION("and back left")
		{
			run(client, "quantize.nudgeCrossfade", {{"events", events}, {"amount", -1.0}});
			CHECK(eventsOn(client, loop.track) == crossfaded);
		}
	}
	SECTION("left")
	{
		run(client, "quantize.nudgeCrossfade", {{"events", events}, {"amount", -1.0}});
		const std::vector<std::vector<int64_t>> nudged = eventsOn(client, loop.track);
		CHECK(nudged[0] == std::vector<int64_t>{0, 0, 6008});
		CHECK(nudged[1] == std::vector<int64_t>{5912, 112, 17896});
	}
	SECTION("not past the start of the clip")
	{
		// The second slice plays from frame 160: 4 ms earlier is the clip's start, so a crossfade can't go there.
		run(client, "quantize.nudgeCrossfade", {{"events", {events.at(1)}}, {"amount", -4.0}});
		CHECK(eventsOn(client, loop.track)[1] == std::vector<int64_t>{5960, 160, 17896});
	}
	SECTION("locked events stay")
	{
		run(client, "event.set", {{"events", {events.at(0)}}, {"lock", true}});
		run(client, "quantize.nudgeCrossfade", {{"events", {events.at(1)}}});
		CHECK(eventsOn(client, loop.track) == crossfaded);
	}
	SECTION("errors")
	{
		CHECK_THAT(reject(client, "quantize.nudgeCrossfade", {{"events", events}, {"amount", 200.0}}),
			ContainsSubstring("-100 to 100"));
		CHECK_THAT(
			reject(client, "quantize.nudgeCrossfade", {{"events", {999}}}), ContainsSubstring("no audio event 999"));
	}
}

TEST_CASE("Quantizing again after Nudge Crossfade Right leaves the nudged slices where they are", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Sliced at the hitpoints with no Offset, a slice starts at its snap point, so the first step right takes its
	// start past it. Quantizing again measures the snap point from the slice's offset before the nudge and moves
	// the result by how far the offset moved since, so the slices stay where they are, playing the same audio
	// there, instead of going back to the grid with the nudged offset (#1581).
	const std::vector<int64_t> played{6200, 25600, 47300, 72800};
	const Loop loop = addLoop(client, dir, "loop", clicks(played, 4 * g_quarter));
	setHitpoints(client, loop.clip, played);
	const Json events = run(client, "quantize.slice", {{"events", {loop.event}}}).at("events");
	const std::vector<std::vector<int64_t>> before = eventsOn(client, loop.track);
	run(client, "quantize.set", {{"preset", "1/8"}});
	run(client, "quantize.apply", {{"events", events}});
	run(client, "quantize.crossfade", {{"events", events}, {"length", 2.0}});
	run(client, "quantize.nudgeCrossfade", {{"events", events}});
	const std::vector<std::vector<int64_t>> nudged = eventsOn(client, loop.track);
	REQUIRE(nudged.at(2) == std::vector<int64_t>{24048, 25648, 21796});

	run(client, "quantize.apply", {{"events", events}});
	CHECK(eventsOn(client, loop.track) == nudged);
	run(client, "quantize.reset", {{"events", events}});
	CHECK(eventsOn(client, loop.track) == before);
}

TEST_CASE("Quantizing again after Nudge Crossfade leaves the nudged slices of a musical track where they are",
	"[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// As on a linear track, but the slices' starts are in ticks at 97 BPM: quantizing again moves the result by how
	// far the offset moved through samples and back into ticks (#1634), which must not take a slice a tick away.
	run(client, "project.set", {{"tempo", 97.0}});
	const std::vector<int64_t> played{6200, 31700, 59300, 89900};
	const Loop loop = addLoop(client, dir, "loop", clicks(played, 5 * g_quarter), 0, "musical");
	setHitpoints(client, loop.clip, played);
	const Json events = run(client, "quantize.slice", {{"events", {loop.event}}}).at("events");
	const std::vector<std::vector<int64_t>> before = eventsOn(client, loop.track);
	run(client, "quantize.set", {{"preset", "1/8"}});
	run(client, "quantize.apply", {{"events", events}});
	run(client, "quantize.crossfade", {{"events", events}, {"length", 2.0}});
	for (const double amount : {1.0, 1.0, -3.0})
		run(client, "quantize.nudgeCrossfade", {{"events", events}, {"amount", amount}});
	const std::vector<std::vector<int64_t>> nudged = eventsOn(client, loop.track);
	REQUIRE(nudged != before);

	run(client, "quantize.apply", {{"events", events}});
	CHECK(eventsOn(client, loop.track) == nudged);
	run(client, "quantize.reset", {{"events", events}});
	CHECK(eventsOn(client, loop.track) == before);
}

TEST_CASE("Create Groove Quantize Preset makes a preset of an audio event's hitpoints", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Hits a little late on the second and fourth quarter of a bar-long loop at bar 2 (reference manual pp. 367, 373).
	constexpr int64_t tick = 1720320; // per quarter
	const int64_t bar = 4 * g_quarter;
	const Loop loop = addLoop(client, dir, "late", clicks({27000, 48000, 75000}, bar), bar);
	setHitpoints(client, loop.clip, {0, 27000, 48000, 75000});
	const Json preset = run(client, "quantize.extractGroove", {{"event", loop.event}}).at("preset");
	CHECK(preset.at("name") == "late"); // the clip's
	const Json& settings = preset.at("settings");
	CHECK(settings.at("origin") == 4 * tick);
	CHECK(settings.at("groove").at("length") == 4 * tick);
	const Json expected = {{{"position", 0}}, {{"position", tick * 27000 / g_quarter}}, {{"position", 2 * tick}},
		{{"position", tick * 75000 / g_quarter}}};
	CHECK(settings.at("groove").at("points") == expected);

	// It quantizes as a MIDI part's groove does: a slice played on the quarter goes where the hit is late.
	run(client, "quantize.set", {{"preset", "late"}});
	const Loop played = addLoop(client, dir, "played", clicks({1000}, g_quarter), bar + g_quarter);
	run(client, "quantize.apply", {{"events", {played.event}}});
	CHECK(positionOf(client, played.event) == bar + 27000);

	SECTION("named")
	{
		CHECK(
			run(client, "quantize.extractGroove", {{"event", loop.event}, {"name", "Drag"}}).at("preset").at("name") ==
			"Drag");
	}
	SECTION("errors")
	{
		CHECK_THAT(
			reject(client, "quantize.extractGroove", {{"event", played.event}}), ContainsSubstring("no hitpoints"));
		CHECK_THAT(reject(client, "quantize.extractGroove", {{"event", 999}}), ContainsSubstring("no audio event 999"));
		CHECK_THAT(reject(client, "quantize.extractGroove", Json::object()), ContainsSubstring("give part or event"));
		CHECK_THAT(reject(client, "quantize.extractGroove", {{"event", loop.event}, {"length", 1000}}),
			ContainsSubstring("length goes with part"));
	}
}

TEST_CASE("Reset of the Slice Rules puts the slices back together into their events", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const std::vector<int64_t> played{6200, 25600, 47300, 72800};
	const Loop close = addLoop(client, dir, "close", clicks(played, 4 * g_quarter));
	const Loop room = addLoop(client, dir, "room", clicks(played, 4 * g_quarter));
	setHitpoints(client, close.clip, played);
	run(client, "event.setSnapPoint", {{"events", {close.event}}, {"position", 3000}});
	const std::vector<std::vector<int64_t>> whole{{0, 0, 4 * g_quarter}};

	// Sliced, quantized and crossfaded, as the panel does it (pp. 370-371).
	const Json sliced =
		run(client, "quantize.slice", {{"events", {close.event, room.event}}, {"offset", 5.0}}).at("events");
	REQUIRE(sliced.size() == 10);
	run(client, "quantize.set", {{"preset", "1/8"}});
	run(client, "quantize.apply", {{"events", sliced}});
	run(client, "quantize.crossfade", {{"events", sliced}, {"length", 2.0}});
	REQUIRE(crossfades(client) == 8);

	SECTION("every slice goes back into its event, which is as it was before slicing")
	{
		const Json result = run(client, "quantize.resetSlices", {{"events", sliced}});
		CHECK(lastStep(client) == "Reset slices");
		CHECK(result.at("events") == Json::array({close.event, room.event}));
		CHECK(result.at("removed").size() == 8);
		CHECK(eventsOn(client, close.track) == whole);
		CHECK(eventsOn(client, room.track) == whole);
		CHECK(byId(client, "audioEvents", close.event).at("snapPoint") == 3000);
		CHECK(byId(client, "audioEvents", room.event).at("snapPoint").is_null());
		CHECK(crossfades(client) == 0);
		// Nothing left to reset, and quantize.reset has nothing either.
		CHECK(run(client, "quantize.resetSlices", {{"events", {close.event}}}).at("events").empty());
		CHECK(run(client, "quantize.reset", {{"events", {close.event}}}).at("events").empty());
		run(client, "history.undo");
		CHECK(eventsOn(client, close.track).size() == 5);
		CHECK(crossfades(client) == 8);
	}

	SECTION("any slice of an event resets all of them, also one split by hand since and after a save and a load")
	{
		const std::vector<std::vector<int64_t>> slices = eventsOn(client, close.track);
		const ObjectId slice = sliced.at(3).get<ObjectId>(); // a slice of close
		const Json found = byId(client, "audioEvents", slice);
		REQUIRE(found.at("track") == close.track);
		run(client, "event.split", {{"events", {slice}}, {"positions", {found.at("position").get<int64_t>() + 1000}}});
		const std::string file = (dir.path() / "sliced.dawproject").u8string();
		run(client, "project.save", {{"path", file}, {"embedMedia", false}});
		run(client, "project.new");
		run(client, "project.load", {{"path", file}});
		const Json result = run(client, "quantize.resetSlices", {{"events", {slice}}});
		CHECK(result.at("events") == Json::array({close.event}));
		CHECK(result.at("removed").size() == 5);
		CHECK(eventsOn(client, close.track) == whole);
		CHECK(eventsOn(client, room.track).size() == 5);
	}

	SECTION("slicing a slice again keeps the event it came from")
	{
		setHitpoints(client, room.clip, {36000});
		run(client, "quantize.slice", {{"events", sliced}});
		run(client, "quantize.resetSlices", {{"events", {room.event}}});
		CHECK(eventsOn(client, room.track) == whole);
	}

	SECTION("a locked event and its slices stay")
	{
		Json closeSlices = Json::array();
		for (const Json& id : sliced)
			if (byId(client, "audioEvents", id.get<ObjectId>()).at("track") == close.track)
				closeSlices.push_back(id);
		run(client, "event.set", {{"events", {closeSlices.at(2)}}, {"lock", true}});
		const Json result = run(client, "quantize.resetSlices", {{"events", sliced}});
		CHECK(result.at("events") == Json::array({room.event}));
		CHECK(eventsOn(client, close.track).size() == 5);
		CHECK(eventsOn(client, room.track) == whole);
	}

	CHECK_THAT(reject(client, "quantize.resetSlices", {{"events", {999}}}), ContainsSubstring("no audio event 999"));
}

TEST_CASE(
	"Reset of the Slice Rules puts back the event's fade out, and its start in the track's time base", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const std::vector<int64_t> played{6200, 25600, 47300, 72800};
	const Loop loop = addLoop(client, dir, "loop", clicks(played, 4 * g_quarter), 2000);
	setHitpoints(client, loop.clip, played);
	run(client, "audioEvent.setFades", {{"event", loop.event}, {"fadeOut", {{"length", 4800}}}});
	const Json fadeOut = byId(client, "audioEvents", loop.event).at("fadeOut");
	const Json sliced = run(client, "quantize.slice", {{"events", {loop.event}}}).at("events");
	REQUIRE(sliced.size() == 5);
	// event.split gave the fade out to the last slice.
	REQUIRE(byId(client, "audioEvents", loop.event).at("fadeOut").at("length") == 0);

	SECTION("the fade out comes back, also after a save and a load")
	{
		const std::string file = (dir.path() / "sliced.dawproject").u8string();
		run(client, "project.save", {{"path", file}, {"embedMedia", false}});
		run(client, "project.new");
		run(client, "project.load", {{"path", file}});
		run(client, "quantize.resetSlices", {{"events", sliced}});
		CHECK(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{2000, 0, 4 * g_quarter}});
		CHECK(byId(client, "audioEvents", loop.event).at("fadeOut") == fadeOut);
	}

	SECTION("a track switched to musical time since slicing: the event starts where it did, in ticks")
	{
		run(client, "track.set", {{"track", loop.track}, {"timeBase", "musical"}});
		run(client, "quantize.resetSlices", {{"events", sliced}});
		CHECK(eventsOn(client, loop.track) ==
			std::vector<std::vector<int64_t>>{{time::g_ticksPerQuarter / 12, 0, 4 * g_quarter}}); // 2000 samples
		CHECK(byId(client, "audioEvents", loop.event).at("fadeOut") == fadeOut);
	}

	SECTION("a track switched to musical time and back since slicing, saved and loaded in between")
	{
		// The origin changes its time base with the event's (#1672), so the file keeps it.
		run(client, "track.set", {{"track", loop.track}, {"timeBase", "musical"}});
		const std::string file = (dir.path() / "switched.dawproject").u8string();
		run(client, "project.save", {{"path", file}, {"embedMedia", false}});
		run(client, "project.new");
		run(client, "project.load", {{"path", file}});
		CHECK(run(client, "quantize.resetSlices", {{"events", sliced}}).at("events") == Json::array({loop.event}));
		CHECK(eventsOn(client, loop.track) ==
			std::vector<std::vector<int64_t>>{{time::g_ticksPerQuarter / 12, 0, 4 * g_quarter}});
		run(client, "history.undo");
		run(client, "track.set", {{"track", loop.track}, {"timeBase", "linear"}});
		run(client, "quantize.resetSlices", {{"events", sliced}});
		CHECK(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{2000, 0, 4 * g_quarter}});
	}

	SECTION("the event and its slices moved onto a musical track, saved and loaded in between")
	{
		// The origin goes into the musical track's time base with the event (#1672), so the file keeps it.
		const ObjectId musical = run(client, "track.add", {{"type", "audio"}, {"timeBase", "musical"}, {"channels", 1}})
									 .at("track")
									 .at("id")
									 .get<ObjectId>();
		run(client, "event.move", {{"events", sliced}, {"delta", 0}, {"track", musical}});
		const std::string file = (dir.path() / "moved.dawproject").u8string();
		run(client, "project.save", {{"path", file}, {"embedMedia", false}});
		run(client, "project.new");
		run(client, "project.load", {{"path", file}});
		CHECK(run(client, "quantize.resetSlices", {{"events", sliced}}).at("events") == Json::array({loop.event}));
		CHECK(eventsOn(client, musical) ==
			std::vector<std::vector<int64_t>>{{time::g_ticksPerQuarter / 12, 0, 4 * g_quarter}}); // 2000 samples
	}

	// The origin moves with the event's audio part (#1728), so the event comes back where the part has it now.
	const auto toPart = [&]()
	{ return run(client, "event.toPart", {{"events", sliced}}).at("events").at(0).get<ObjectId>(); };

	SECTION("the event's audio part moved a bar later")
	{
		const ObjectId part = toPart();
		run(client, "event.move", {{"events", {part}}, {"delta", 4 * g_quarter}});
		CHECK(run(client, "quantize.resetSlices", {{"events", sliced}}).at("events") == Json::array({loop.event}));
		CHECK(eventsOn(client, loop.track) ==
			std::vector<std::vector<int64_t>>{{(4 * g_quarter) + 2000, 0, 4 * g_quarter}});
		CHECK(byId(client, "audioEvents", loop.event).at("part") == part);
	}

	SECTION("the event's audio part's start sized a bar later, moving its contents")
	{
		const ObjectId part = toPart();
		run(client, "event.resize",
			{{"events", {part}}, {"edge", "start"}, {"delta", 4 * g_quarter}, {"mode", "moveContents"}});
		// Where the first slice went with the contents, a bar later within the rounding of a tick.
		const int64_t start = eventsOn(client, loop.track).at(0).at(0);
		REQUIRE(std::abs(start - ((4 * g_quarter) + 2000)) <= 1);
		CHECK(run(client, "quantize.resetSlices", {{"events", sliced}}).at("events") == Json::array({loop.event}));
		CHECK(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{start, 0, 4 * g_quarter}});
		CHECK(byId(client, "audioEvents", loop.event).at("part") == part);
	}

	SECTION("the event's audio part moved a bar later onto a musical track, saved and loaded in between")
	{
		const ObjectId musical = run(client, "track.add", {{"type", "audio"}, {"timeBase", "musical"}, {"channels", 1}})
									 .at("track")
									 .at("id")
									 .get<ObjectId>();
		run(client, "event.move", {{"events", {toPart()}}, {"delta", 4 * g_quarter}, {"track", musical}});
		const std::string file = (dir.path() / "part.dawproject").u8string();
		run(client, "project.save", {{"path", file}, {"embedMedia", false}});
		run(client, "project.new");
		run(client, "project.load", {{"path", file}});
		CHECK(run(client, "quantize.resetSlices", {{"events", sliced}}).at("events") == Json::array({loop.event}));
		CHECK(eventsOn(client, musical) ==
			std::vector<std::vector<int64_t>>{
				{(4 * time::g_ticksPerQuarter) + (time::g_ticksPerQuarter / 12), 0, 4 * g_quarter}});
		CHECK(byId(client, "audioEvents", loop.event).at("fadeOut") == fadeOut);
	}
}

TEST_CASE("Copies of slices have no slice origin of their own, but a duplicated track's slices do", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const std::vector<int64_t> played{6200, 25600, 47300, 72800};
	const Loop loop = addLoop(client, dir, "loop", clicks(played, 4 * g_quarter));
	setHitpoints(client, loop.clip, played);
	const Json sliced = run(client, "quantize.slice", {{"events", {loop.event}}}).at("events");
	REQUIRE(sliced.size() == 5);
	const std::vector<std::vector<int64_t>> slices = eventsOn(client, loop.track);
	const std::vector<std::vector<int64_t>> whole{{0, 0, 4 * g_quarter}};

	SECTION("a copy of the first slice and of another one are the user's own (#1668)")
	{
		const Json copies = run(client, "event.duplicate", {{"events", {loop.event, sliced.at(2)}}}).at("events");
		REQUIRE(copies.size() == 2);
		for (const Json& copy : copies)
		{
			const Json event = byId(client, "audioEvents", copy.get<ObjectId>());
			CHECK(!event.contains("sliceOrigin"));
			CHECK(!event.contains("slicedFrom"));
		}
		CHECK(run(client, "quantize.resetSlices", {{"events", copies}}).at("events").empty());
		run(client, "quantize.resetSlices", {{"events", sliced}});
		std::vector<std::vector<int64_t>> left = eventsOn(client, loop.track);
		CHECK(left.size() == 3); // the event, whole again, and the two copies
		CHECK(left.at(0) == whole.at(0));
	}

	SECTION("a duplicated track resets its own slices")
	{
		const ObjectId copy =
			run(client, "track.duplicate", {{"track", loop.track}}).at("track").at("id").get<ObjectId>();
		Json copied = Json::array();
		const Json project = run(client, "project.get");
		for (const Json& event : project.at("audioEvents"))
			if (event.at("track") == copy)
				copied.push_back(event.at("id"));
		REQUIRE(copied.size() == 5);
		const Json result = run(client, "quantize.resetSlices", {{"events", Json::array({copied.at(3)})}});
		CHECK(result.at("removed").size() == 4);
		CHECK(eventsOn(client, copy) == whole);
		CHECK(eventsOn(client, loop.track) == slices);
	}

	SECTION("an imported track resets its own slices, and the project's own stay (#1728)")
	{
		const std::string file = (dir.path() / "sliced.dawproject").u8string();
		run(client, "project.save", {{"path", file}, {"embedMedia", false}});
		const ObjectId imported = run(client, "track.import", {{"path", file}}).at("tracks").at(0).get<ObjectId>();
		REQUIRE(imported != loop.track);
		Json copied = Json::array();
		const Json project = run(client, "project.get");
		for (const Json& event : project.at("audioEvents"))
			if (event.at("track") == imported)
				copied.push_back(event.at("id"));
		REQUIRE(copied.size() == 5);
		const Json result = run(client, "quantize.resetSlices", {{"events", Json::array({copied.at(3)})}});
		CHECK(result.at("removed").size() == 4);
		CHECK(eventsOn(client, imported) == whole);
		CHECK(eventsOn(client, loop.track) == slices);
	}

	SECTION("a duplicated version resets its own slices (#1668)")
	{
		run(client, "trackVersion.duplicate", {{"tracks", {loop.track}}});
		Json copied = Json::array();
		const Json project = run(client, "project.get");
		for (const Json& event : project.at("audioEvents"))
			if (event.at("track") == loop.track)
				copied.push_back(event.at("id"));
		REQUIRE(copied.size() == 5);
		for (const Json& id : copied)
			REQUIRE(std::find(sliced.begin(), sliced.end(), id) == sliced.end());
		const Json result = run(client, "quantize.resetSlices", {{"events", Json::array({copied.at(3)})}});
		CHECK(result.at("removed").size() == 4);
		CHECK(eventsOn(client, loop.track) == whole);
	}
}

TEST_CASE("A copied track resets its quantized events to crossfades from the copies of their events", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// The first event crossfades into the second by hand, and the quantize panel's Crossfade step crossfades the
	// second into the third. A copy's quantize origins name the copies of the events (#2136), so resetting the copy
	// puts its hand-made crossfade back, from the copy of the first event, and removes the one the step made.
	const Track loop = addTrack(client, dir, "loop", clicks({1000}, 4 * g_quarter));
	const ObjectId first = loop.add(client, 1000, 0, 30000);
	const ObjectId second = loop.add(client, 25000, 24000, 30000);
	const ObjectId third = loop.add(client, 49000, 48000, 30000);
	run(client, "crossfade.set", {{"from", first}, {"to", second}, {"mode", "equalPower"}});
	const std::vector<std::vector<int64_t>> before = eventsOn(client, loop.track);
	run(client, "quantize.set", {{"preset", "1/4"}});
	const Json events = Json::array({first, second, third});
	run(client, "quantize.apply", {{"events", events}});
	run(client, "quantize.crossfade", {{"events", events}});
	REQUIRE(crossfadeOf(client, third).at("from") == second);

	const std::string how = GENERATE(as<std::string>{}, "track.duplicate", "trackVersion.duplicate", "track.import");
	CAPTURE(how);
	const ObjectId track = copyOf(client, dir, loop.track, how);
	const std::vector<ObjectId> copies = idsOn(client, track);
	REQUIRE(copies.size() == 3);
	for (const ObjectId copy : copies)
		REQUIRE((copy != first && copy != second && copy != third));
	run(client, "quantize.reset", {{"events", copies}});
	CHECK(eventsOn(client, track) == before);
	const Json kept = crossfadeOf(client, copies.at(1));
	REQUIRE(kept.is_object());
	CHECK(kept.at("from") == copies.at(0));
	CHECK(kept.at("mode") == "equalPower");
	CHECK(!crossfadeOf(client, copies.at(2)).is_object());
}

TEST_CASE("A copied track resets its slices to the crossfade from the copy of the event before", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// The first event crossfades into the second, which quantize.slice then cuts: its slice origin keeps the
	// crossfade. A copy's slice origin names the copy of the first event (#2136), so the Reset of the Slice Rules on
	// the copy puts the crossfade back from there.
	const std::vector<int64_t> played{6200, 25600, 47300, 72800};
	const Track loop = addTrack(client, dir, "loop", clicks(played, 4 * g_quarter));
	setHitpoints(client, loop.clip, played);
	const ObjectId first = loop.add(client, 0, 0, 40000);
	const ObjectId second = loop.add(client, 36000, 36000, 60000);
	run(client, "crossfade.set", {{"from", first}, {"to", second}, {"mode", "equalPower"}});
	const std::vector<std::vector<int64_t>> before = eventsOn(client, loop.track);
	REQUIRE(run(client, "quantize.slice", {{"events", {second}}}).at("events").size() == 3);

	const std::string how = GENERATE(as<std::string>{}, "track.duplicate", "trackVersion.duplicate", "track.import");
	CAPTURE(how);
	const ObjectId track = copyOf(client, dir, loop.track, how);
	const std::vector<ObjectId> copies = idsOn(client, track);
	REQUIRE(copies.size() == 4);
	for (const ObjectId copy : copies)
		REQUIRE(copy != first);
	run(client, "quantize.resetSlices", {{"events", copies}});
	CHECK(eventsOn(client, track) == before);
	const std::vector<ObjectId> reset = idsOn(client, track);
	REQUIRE(reset.size() == 2);
	const Json kept = crossfadeOf(client, reset.at(1));
	REQUIRE(kept.is_object());
	CHECK(kept.at("from") == reset.at(0));
	CHECK(kept.at("mode") == "equalPower");
}

TEST_CASE("Reset of the Warp Marker Creation Rules removes the markers Create made", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const Loop a = addLoop(client, dir, "a", clicks({1000, 25600}, 2 * g_quarter));
	setHitpoints(client, a.clip, {1000, 25600});
	run(client, "clip.addWarpMarker", {{"clip", a.clip}, {"position", 40000}});
	const Json mine = markersOf(client, a.clip);
	REQUIRE(mine.size() == 1);
	run(client, "quantize.createWarpMarkers", {{"events", {a.event}}});
	REQUIRE(markersOf(client, a.clip).size() == 3);

	SECTION("the user's own marker stays")
	{
		const Json result = run(client, "quantize.resetWarpMarkers", {{"events", {a.event}}});
		CHECK(lastStep(client) == "Reset warp markers");
		CHECK(result.at("clips") == Json::array({a.clip}));
		CHECK(markersOf(client, a.clip) == mine);
		CHECK(run(client, "quantize.resetWarpMarkers", {{"events", {a.event}}}).at("clips").empty());
		run(client, "history.undo");
		CHECK(markersOf(client, a.clip).size() == 3);
	}

	SECTION("also once they are warp quantized, and across a save and a load; quantize.reset then puts back the rest")
	{
		run(client, "quantize.set", {{"preset", "1/4"}, {"settings", {{"warpQuantize", true}}}});
		run(client, "quantize.apply", {{"events", {a.event}}});
		const std::string file = (dir.path() / "markers.dawproject").u8string();
		run(client, "project.save", {{"path", file}, {"embedMedia", false}});
		run(client, "project.new");
		run(client, "project.load", {{"path", file}});
		run(client, "quantize.resetWarpMarkers", {{"events", {a.event}}});
		const Json left = markersOf(client, a.clip);
		CHECK(std::none_of(left.begin(), left.end(),
			[](const Json& marker) { return marker.at("position") == 1000 || marker.at("position") == 25600; }));
		CHECK(std::any_of(left.begin(), left.end(), [](const Json& marker) { return marker.at("position") == 40000; }));
		run(client, "quantize.reset", {{"events", {a.event}}});
		CHECK(markersOf(client, a.clip) == mine);
	}

	SECTION("a locked event's clip keeps them")
	{
		run(client, "event.set", {{"events", {a.event}}, {"lock", true}});
		CHECK(run(client, "quantize.resetWarpMarkers", {{"events", {a.event}}}).at("clips").empty());
		CHECK(markersOf(client, a.clip).size() == 3);
	}

	SECTION("a marker the user adds where a removed one was made is the user's own (#1668)")
	{
		const auto check = [&]()
		{
			run(client, "clip.addWarpMarker", {{"clip", a.clip}, {"position", 25600}});
			CHECK(run(client, "quantize.resetWarpMarkers", {{"events", {a.event}}}).at("clips").empty());
			CHECK(markersOf(client, a.clip).size() == 2);
		};
		SECTION("removed one by one")
		{
			run(client, "clip.removeWarpMarkers", {{"clip", a.clip}, {"positions", {1000, 25600}}});
			check();
		}
		SECTION("all removed")
		{
			run(client, "clip.removeWarpMarkers", {{"clip", a.clip}});
			run(client, "clip.addWarpMarker", {{"clip", a.clip}, {"position", 40000}});
			check();
		}
		SECTION("unstretched")
		{
			run(client, "event.unstretch", {{"events", {a.event}}});
			run(client, "clip.addWarpMarker", {{"clip", a.clip}, {"position", 40000}});
			check();
		}
	}
}

TEST_CASE("Warp quantizing a selection range moves only the warp markers inside it", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 256}, {"inputChannels", 0}, {"captureSeconds", 6}});
	// As in the whole event's test: beats 2 to 4 a little off, an eighth between beats 2 and 3.
	const Loop loop = addLoop(client, dir, "loop", clicks({1000, 25000, 36500, 47000, 73000}, 4 * g_quarter));
	setHitpoints(client, loop.clip, {25000, 36500, 47000, 73000});
	run(client, "quantize.set", {{"preset", "1/4"}, {"settings", {{"warpQuantize", true}}}});

	// The range from 12000 to 60000 (p. 370): the hitpoint at 73000 is outside it and stays where it plays.
	const Json result = run(client, "quantize.apply", {{"events", {loop.event}}, {"start", 12000}, {"end", 60000}});
	CHECK(result.at("clips") == Json::array({loop.clip}));
	const Json markers = markersOf(client, loop.clip);
	REQUIRE(markers.size() == 5);
	CHECK(markers.front() == Json{{"position", 12000}, {"target", 12000}});
	CHECK(markers.back() == Json{{"position", 60000}, {"target", 60000}});
	const double eighth = g_quarter + (11500.0 * 24000.0 / 22000.0);
	checkClicks(render(client, 4 * g_quarter), {1000.0, 24000.0, eighth, 48000.0, 73000.0});

	SECTION("a range outside the event changes nothing")
	{
		run(client, "quantize.reset", {{"events", {loop.event}}});
		CHECK(
			run(client, "quantize.apply", {{"events", {loop.event}}, {"start", 5 * g_quarter}, {"end", 6 * g_quarter}})
				.at("clips")
				.empty());
		CHECK(markersOf(client, loop.clip).empty());
	}

	SECTION("another range keeps what the first one quantized, and reset puts all back")
	{
		run(client, "quantize.apply", {{"events", {loop.event}}, {"start", 60000}, {"end", 90000}});
		checkClicks(render(client, 4 * g_quarter), {1000.0, 24000.0, eighth, 48000.0, 72000.0});
		run(client, "quantize.reset", {{"events", {loop.event}}});
		CHECK(markersOf(client, loop.clip).empty());
	}

	CHECK_THAT(reject(client, "quantize.apply", {{"events", {loop.event}}, {"start", 12000}, {"end", 12000}}),
		ContainsSubstring("end after start"));
	CHECK_THAT(reject(client, "quantize.apply",
				   {{"events", {loop.event}}, {"start", 0}, {"end", 12000}, {"settings", {{"warpQuantize", false}}}}),
		ContainsSubstring("warpQuantize"));
	CHECK_THAT(
		reject(client, "quantize.apply", {{"events", {loop.event}}, {"start", 0}}), ContainsSubstring("start and end"));
}

TEST_CASE("The cutting positions are where Slice would cut", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const std::vector<int64_t> played{6200, 25600, 47300, 72800};
	const Loop close = addLoop(client, dir, "close", clicks(played, 4 * g_quarter));
	const Loop room = addLoop(client, dir, "room", clicks(played, 4 * g_quarter));
	// Without hitpoints there is nothing to show, which is no error.
	CHECK(run(client, "quantize.slicePositions", {{"events", {close.event, room.event}}}).at("positions").empty());
	setHitpoints(client, close.clip, played);
	setHitpoints(client, room.clip, {6300, 60000});
	const Json parameters = {{"events", {close.event, room.event}},
		{"priorities", {{{"track", close.track}, {"priority", 2}}}}, {"range", 20.0}, {"offset", 5.0}};
	const size_t steps = run(client, "history.get").at("steps").size();
	const Json positions = run(client, "quantize.slicePositions", parameters).at("positions");
	CHECK(positions == Json::array({5960, 25360, 47060, 59760, 72560}));
	CHECK(run(client, "history.get").at("steps").size() == steps);
	CHECK(eventsOn(client, close.track).size() == 1);
	// Slice cuts there.
	run(client, "quantize.slice", parameters);
	std::vector<int64_t> starts = startsOn(client, close.track);
	starts.erase(starts.begin());
	CHECK(Json(starts) == positions);
	CHECK_THAT(reject(client, "quantize.slicePositions", {{"events", {999}}}), ContainsSubstring("no audio event 999"));
}

TEST_CASE("Group Editing is a folder track's mode, saved with the project", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const ObjectId audio =
		run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id").get<ObjectId>();
	const ObjectId folder =
		run(client, "track.add", {{"type", "folder"}, {"tracks", {audio}}}).at("track").at("id").get<ObjectId>();
	CHECK(byId(client, "tracks", folder).at("groupEditing") == false);
	const Json result = run(client, "track.set", {{"track", folder}, {"groupEditing", true}});
	CHECK(result.at("track").at("groupEditing") == true);
	run(client, "history.undo");
	CHECK(byId(client, "tracks", folder).at("groupEditing") == false);
	run(client, "history.redo");
	const std::string file = (dir.path() / "group.dawproject").u8string();
	run(client, "project.save", {{"path", file}, {"embedMedia", false}});
	run(client, "project.new");
	run(client, "project.load", {{"path", file}});
	CHECK(byId(client, "tracks", folder).at("groupEditing") == true);
	CHECK_THAT(reject(client, "track.set", {{"track", audio}, {"groupEditing", true}}),
		ContainsSubstring("only folder tracks"));
}
