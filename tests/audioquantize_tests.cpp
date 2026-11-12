// SPDX-License-Identifier: GPL-3.0-or-later
// Audio quantizing (#145, docs/features/audio-quantize.md, reference manual pp. 367-380): audio event starts (their
// snap points) to the grid, warp quantizing of warp markers, and several audio tracks sliced at the hitpoints of
// the track with the highest priority, quantized and crossfaded so that they stay phase coherent.
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

	// The audio as a clip, and an event of it at position on a new linear audio track.
	Loop addLoop(engine::Client& client, const test::TempDir& dir, const std::string& name, const test::Audio& audio,
		int64_t position = 0)
	{
		const std::string file = (dir.path() / (name + ".wav")).u8string();
		test::writeWav(file, audio);
		Loop loop;
		loop.track = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 1}})
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

	std::string lastStep(engine::Client& client)
	{
		return run(client, "history.get").at("steps").back().at("action").get<std::string>();
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
	CHECK(byId(client, "audioEvents", right).at("quantizeOrigin") == Json({{"original", 14000}, {"quantized", 12000}}));

	run(client, "quantize.reset", {{"events", {loop.event, right}}});
	CHECK(positionOf(client, loop.event) == 2000);
	CHECK(positionOf(client, right) == 14000);
	CHECK(eventsOn(client, loop.track) == std::vector<std::vector<int64_t>>{{2000, 0, 12000}, {14000, 12000, 84000}});
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
