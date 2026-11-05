// SPDX-License-Identifier: GPL-3.0-or-later
// Tempo matching and warping (#144, docs/features/warp.md): dsp::WarpReader on clicks, musical mode following tempo
// changes in a render, the definition grid from hitpoints and by hand, free warp markers, unstretching, flattening and
// saving.
#include "engine/audiofile/audiofile.h"
#include "engine/document/rendersnapshot.h"
#include "engine/dsp/warp.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;
using engine::ObjectId;
using time::g_ticksPerQuarter;

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

	// 5 ms of 1.5 kHz under a Hann window centred on each of the frames, mono at 48 kHz.
	test::Audio clicks(const std::vector<int64_t>& at, int64_t frames)
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
					static_cast<float>(0.8 * envelope * std::sin(phase));
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

	// Checks that a click is within 10 ms of each of the frames.
	void checkClicks(const std::vector<float>& samples, const std::vector<double>& expected)
	{
		for (const double at : expected)
		{
			const double offset = centre(samples, at) - at;
			CAPTURE(at, offset);
			CHECK(std::abs(offset) <= 480.0);
		}
	}

	// The loop as a clip, and an event of it at the project start on a new audio track.
	struct Loop
	{
		ObjectId track = 0;
		ObjectId clip = 0;
		ObjectId event = 0;
	};

	Loop addLoop(engine::Client& client, const test::TempDir& dir, const test::Audio& audio,
		const std::string& timeBase = "musical")
	{
		const std::string file = (dir.path() / "loop.wav").u8string();
		test::writeWav(file, audio);
		Loop loop;
		loop.track = run(client, "track.add", {{"type", "audio"}, {"timeBase", timeBase}, {"channels", 1}})
						 .at("track")
						 .at("id")
						 .get<ObjectId>();
		const Json media = run(client, "media.add", {{"path", file}}).at("media");
		loop.clip = run(client, "clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<ObjectId>();
		loop.event = run(client, "audioEvent.add",
			{{"track", loop.track}, {"clip", loop.clip}, {"position", 0}, {"offset", 0},
				{"length", static_cast<int64_t>(audio.frames())}})
						 .at("event")
						 .at("id")
						 .get<ObjectId>();
		return loop;
	}

	// The left channel of the main mix rendered offline from the project start, by a new dummy device, which
	// captures from its own start.
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

	// The object of a project.get list with the id, or null.
	Json byId(engine::Client& client, const char* list, ObjectId id)
	{
		const Json project = run(client, "project.get");
		for (const Json& each : project.at(list))
			if (each.at("id") == id)
				return each;
		return nullptr;
	}

	Json warpOf(engine::Client& client, ObjectId clip)
	{
		const Json found = byId(client, "clips", clip);
		REQUIRE(found.is_object());
		return found.at("warp");
	}

	Json eventOf(engine::Client& client, ObjectId event)
	{
		Json found = byId(client, "audioEvents", event);
		REQUIRE(found.is_object());
		return found;
	}
}

TEST_CASE("a warp reader plays clicks where its map puts them", "[unit]")
{

	const test::TempDir dir;
	const std::string file = (dir.path() / "clicks.wav").u8string();
	test::writeWav(file, clicks({6000, 18000, 30000, 42000}, 48000));
	// Twice as long up to frame 24000, then as fast as the file.
	const std::vector<engine::dsp::WarpPoint> map{{0.0, 0.0}, {24000.0, 48000.0}, {48000.0, 72000.0}};
	CHECK(engine::dsp::warpTarget(map, 12000.0) == 24000.0);
	CHECK(engine::dsp::warpTarget(map, 36000.0) == 60000.0);
	CHECK(engine::dsp::warpSource(map, 60000.0) == 36000.0);
	CHECK(engine::dsp::warpTarget(map, 60000.0) == 84000.0); // the last segment goes on
	engine::dsp::WarpReader reader(engine::AudioFileReader::open(file), map, engine::dsp::StretchPreset::Quality);
	REQUIRE(reader.info().frames == 72000);
	std::vector<float> output(72000);
	size_t done = 0;
	while (done < output.size())
	{
		const size_t got = reader.read(output.data() + done, std::min<size_t>(4096, output.size() - done));
		REQUIRE(got > 0);
		done += got;
	}
	CHECK(reader.read(output.data(), 1) == 0);
	checkClicks(output, {12000.0, 36000.0, 54000.0, 66000.0});

	// After a seek it plays the same clicks from there.
	reader.seek(40000);
	std::vector<float> after(32000);
	REQUIRE(reader.read(after.data(), after.size()) == after.size());
	std::vector<float> placed(40000, 0.0F);
	placed.insert(placed.end(), after.begin(), after.end());
	checkClicks(placed, {54000.0, 66000.0});
	CHECK_THROWS(engine::dsp::WarpReader(
		engine::AudioFileReader::open(file), {{0.0, 0.0}}, engine::dsp::StretchPreset::Quality));
}

TEST_CASE("audio in musical mode follows tempo changes", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 256}, {"inputChannels", 0}, {"captureSeconds", 6}});
	// Four quarters at 120 BPM with a click on each.
	const Loop loop = addLoop(client, dir, clicks({1000, 25000, 49000, 73000}, 4 * g_quarter));
	const std::vector<double> straight{1000.0, 25000.0, 49000.0, 73000.0};

	// Without musical mode a tempo change leaves the audio as it is.
	run(client, "project.set", {{"tempo", 60.0}});
	checkClicks(render(client, 6 * g_quarter), straight);

	// Musical mode takes the tempo where its event starts, so turning it on changes nothing until the tempo changes.
	Json result = run(client, "clip.setWarp", {{"clip", loop.clip}, {"musical", true}});
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Set musical mode");
	CHECK(result.at("warp").at("musical") == true);
	CHECK(result.at("warp").at("tempo") == 60.0);
	run(client, "clip.setWarp", {{"clip", loop.clip}, {"tempo", 120.0}});
	// At 60 BPM each quarter takes twice as long.
	std::vector<float> slow = render(client, 10 * g_quarter);
	checkClicks(slow, {2000.0, 50000.0, 98000.0, 146000.0});
	CHECK(eventOf(client, loop.event).at("length") == 4 * g_quarter); // in clip frames, as before
	// Undo brings back the tempo the clip took, at which it plays as recorded; redo the tempo again.
	run(client, "history.undo");
	CHECK(warpOf(client, loop.clip).at("tempo") == 60.0);
	checkClicks(render(client, 6 * g_quarter), straight);
	run(client, "history.redo");

	// A tempo track: 120 BPM for two quarters, then 240.
	run(client, "project.set", {{"tempoTrackActive", true}});
	run(client, "tempo.add", {{"position", 2 * g_ticksPerQuarter}, {"bpm", 240.0}, {"ramp", false}});
	checkClicks(render(client, 4 * g_quarter), {1000.0, 25000.0, 48000.0 + 500.0, 60000.0 + 500.0});
	// The event ends where its last quarter does.
	const engine::RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
	for (const engine::RenderChannel& channel : snapshot.channels)
		for (const engine::RenderAudioEvent& event : channel.audioEvents)
			if (event.id == loop.event)
				CHECK(std::abs(event.length - (3 * g_quarter)) <= 1);

	// Off again, it plays as recorded.
	run(client, "clip.setWarp", {{"clip", loop.clip}, {"musical", false}});
	checkClicks(render(client, 6 * g_quarter), straight);
	CHECK_THAT(reject(client, "clip.setWarp", {{"clip", loop.clip}, {"tempo", 0.5}}), ContainsSubstring("1 to 999"));
	CHECK_THAT(reject(client, "clip.setWarp", {{"clip", 999}}), ContainsSubstring("no clip"));
}

TEST_CASE("bars and beats, Auto Adjust and Manual Adjust define a clip's grid", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Two bars at 100 BPM (28800 frames a quarter), the third quarter's click 1000 frames late.
	const int64_t beat = 28800;
	std::vector<int64_t> at;
	for (int64_t q = 0; q < 8; ++q)
		at.push_back(500 + (q * beat) + (q == 2 ? 1000 : 0));
	const Loop loop = addLoop(client, dir, clicks(at, (8 * beat) + 1000), "linear");

	// Two bars long: eight quarters in its frames.
	Json warp = run(client, "clip.setWarp", {{"clip", loop.clip}, {"bars", 2}}).at("warp");
	CHECK(std::abs(warp.at("tempo").get<double>() - (8.0 * 60.0 * 48000.0 / ((8.0 * beat) + 1000.0))) < 1e-9);
	CHECK(warp.at("musical") == false);
	CHECK_THAT(reject(client, "clip.setWarp", {{"clip", loop.clip}, {"bars", 0}}), ContainsSubstring("length"));
	CHECK_THAT(reject(client, "clip.setWarp", {{"clip", loop.clip}, {"beatsPerBar", 0}}), ContainsSubstring("1 to 64"));

	// Auto Adjust needs hitpoints.
	CHECK_THAT(reject(client, "clip.autoAdjust", {{"clip", loop.clip}}), ContainsSubstring("hitpoints"));
	Json hitpoints = Json::array();
	for (const int64_t click : at)
		hitpoints.push_back({{"position", click - 120}, {"peak", -2.0}, {"intensity", 1.0}});
	run(client, "clip.setHitpoints", {{"clip", loop.clip}, {"hitpoints", hitpoints}});
	// From the first click to the end, two bars: the late click's beat is edited onto it.
	warp = run(client, "clip.autoAdjust", {{"clip", loop.clip}, {"start", 380}, {"end", (8 * beat) + 380}}).at("warp");
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Auto adjust");
	CHECK(std::abs(warp.at("tempo").get<double>() - 100.0) < 1e-9);
	CHECK(warp.at("gridStart") == 380);
	REQUIRE(warp.at("beats").size() == 1);
	CHECK(warp.at("beats").at(0) == Json{{"beat", 2}, {"frame", 380 + (2 * beat) + 1000}});

	// Manual Adjust.
	warp = run(client, "clip.adjustGrid", {{"clip", loop.clip}, {"mode", "start"}, {"frame", 400}}).at("warp");
	CHECK(warp.at("gridStart") == 400);
	CHECK(warp.at("beats").at(0).at("frame") == 400 + (2 * beat) + 1000); // moved with it
	warp = run(client, "clip.adjustGrid",
		{{"clip", loop.clip}, {"mode", "beat"}, {"beat", 5}, {"frame", 400 + (5 * beat) + 300}})
			   .at("warp");
	REQUIRE(warp.at("beats").size() == 2);
	// Set Bar Position at the second bar moves the beats edited after it with it.
	warp = run(client, "clip.adjustGrid",
		{{"clip", loop.clip}, {"mode", "bar"}, {"beat", 4}, {"frame", 400 + (4 * beat) + 100}})
			   .at("warp");
	REQUIRE(warp.at("beats").size() == 3);
	CHECK(warp.at("beats").at(1) == Json{{"beat", 4}, {"frame", 400 + (4 * beat) + 100}});
	// Beat 4 was between the edits of beats 2 and 5, at 400 + 4 * beat + 533: beat 5 moves by -433 with it.
	CHECK(warp.at("beats").at(2) == Json{{"beat", 5}, {"frame", 400 + (5 * beat) + 300 - 433}});
	warp = run(client, "clip.adjustGrid", {{"clip", loop.clip}, {"mode", "remove"}, {"beat", 5}}).at("warp");
	CHECK(warp.at("beats").size() == 2);
	CHECK_THAT(reject(client, "clip.adjustGrid", {{"clip", loop.clip}, {"mode", "remove"}, {"beat", 5}}),
		ContainsSubstring("isn't edited"));
	CHECK_THAT(reject(client, "clip.adjustGrid", {{"clip", loop.clip}, {"mode", "beat"}, {"beat", 3}, {"frame", 10}}),
		ContainsSubstring("must come after"));
	// Stretch Grid: the second bar at 4 * 24000 frames from the grid start makes 120 BPM, and drops the edits.
	warp = run(client, "clip.adjustGrid", {{"clip", loop.clip}, {"mode", "stretch"}, {"frame", 400 + (4 * g_quarter)}})
			   .at("warp");
	CHECK(std::abs(warp.at("tempo").get<double>() - 120.0) < 1e-9);
	CHECK(warp.at("beats").empty());
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Adjust grid");
}

TEST_CASE("an edited beat plays on the project's beat in musical mode", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 256}, {"inputChannels", 0}, {"captureSeconds", 6}});
	// Clicks at 120 BPM, the third one 3000 frames late, and the beat edited onto it.
	const Loop loop = addLoop(client, dir, clicks({1000, 25000, 52000, 73000}, 4 * g_quarter));
	run(client, "clip.setWarp", {{"clip", loop.clip}, {"musical", true}, {"gridStart", 1000}});
	run(client, "clip.adjustGrid", {{"clip", loop.clip}, {"mode", "beat"}, {"beat", 2}, {"frame", 52000}});
	// Beat 0 is at clip frame 1000, beat 2 at 52000: the clicks between play at 24000 / 25500 of their distance
	// from the grid start, those after it 3000 frames early, where the beats are.
	checkClicks(render(client, 4 * g_quarter), {1000.0, 1000.0 + (24000.0 * 24000.0 / 25500.0), 49000.0, 70000.0});
}

TEST_CASE("free warp markers move the audio between them", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const Loop loop = addLoop(client, dir, clicks({1000, 25000, 52000, 73000}, 4 * g_quarter), "linear");

	// The late click moves back onto its beat; the audio before and after stretches to fit.
	Json warp =
		run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 52000}, {"target", 49000}}).at("warp");
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Insert warp marker");
	CHECK(warp.at("markers") == Json::array({{{"position", 52000}, {"target", 49000}}}));
	CHECK(warp.at("musical") == false);
	checkClicks(render(client, 4 * g_quarter),
		{1000.0 * 49000.0 / 52000.0, 25000.0 * 49000.0 / 52000.0, 49000.0, 49000.0 + (21000.0 * 47000.0 / 44000.0)});

	run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 25000}});
	warp = run(client, "clip.setWarpMarker", {{"clip", loop.clip}, {"position", 25000}, {"target", 24000}}).at("warp");
	CHECK(warp.at("markers").size() == 2);
	CHECK(warp.at("markers").at(0) == Json{{"position", 25000}, {"target", 24000}});
	CHECK_THAT(reject(client, "clip.setWarpMarker", {{"clip", loop.clip}, {"position", 25000}, {"target", 50000}}),
		ContainsSubstring("pass each other"));
	CHECK_THAT(
		reject(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 25000}}), ContainsSubstring("already"));
	CHECK_THAT(reject(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 0}}),
		ContainsSubstring("within the clip"));
	CHECK_THAT(reject(client, "clip.setWarpMarker", {{"clip", loop.clip}, {"position", 7}, {"target", 8}}),
		ContainsSubstring("no warp marker"));
	warp = run(client, "clip.removeWarpMarkers", {{"clip", loop.clip}, {"positions", {25000}}}).at("warp");
	CHECK(warp.at("markers").size() == 1);
	// Reset removes them all.
	warp = run(client, "clip.removeWarpMarkers", {{"clip", loop.clip}}).at("warp");
	CHECK(warp.at("markers").empty());
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Remove warp markers");
}

TEST_CASE("unstretching and flattening end real-time warping", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const Loop loop = addLoop(client, dir, clicks({1000, 25000, 49000, 73000}, 4 * g_quarter));
	CHECK_THAT(reject(client, "event.flatten", {{"events", {loop.event}}}), ContainsSubstring("warped"));
	run(client, "clip.setWarp", {{"clip", loop.clip}, {"musical", true}});
	run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 49000}, {"target", 50000}});
	run(client, "project.set", {{"tempo", 80.0}});
	const std::vector<float> warped = render(client, 6 * g_quarter);
	// The marker plays clip frame 49000 at 50000, then each quarter takes one and a half of the clip's.
	const auto at = [](double frame)
	{ return 1.5 * (frame < 49000.0 ? frame * 50000.0 / 49000.0 : 50000.0 + ((frame - 49000.0) * 46000.0 / 47000.0)); };
	const std::vector<double> expected{at(1000.0), at(25000.0), at(49000.0), at(73000.0)};
	checkClicks(warped, expected);

	SECTION("unstretch")
	{
		const Json result = run(client, "event.unstretch", {{"events", {loop.event}}});
		CHECK(result.at("clips") == Json::array({loop.clip}));
		CHECK(run(client, "history.get").at("steps").back().at("action") == "Unstretch audio");
		const Json warp = warpOf(client, loop.clip);
		CHECK(warp.at("musical") == false);
		CHECK(warp.at("markers").empty());
		CHECK(warp.at("tempo") == 120.0); // the definition stays
		checkClicks(render(client, 4 * g_quarter), {1000.0, 25000.0, 49000.0, 73000.0});
	}
	SECTION("flatten")
	{
		const Json result = run(client, "event.flatten", {{"events", {loop.event}}});
		CHECK(run(client, "history.get").at("steps").back().at("action") == "Flatten real-time processing");
		REQUIRE(result.at("clips").size() == 1);
		const auto clip = result.at("clips").at(0).get<ObjectId>();
		const Json event = eventOf(client, loop.event);
		CHECK(event.at("clip") == clip);
		CHECK(event.at("offset") == 0);
		CHECK(std::abs(event.at("length").get<int64_t>() - (4 * 36000)) <= 1); // four quarters at 80 BPM
		CHECK(warpOf(client, clip).at("musical") == false);
		// It plays as it did, and no longer follows the tempo.
		const std::vector<float> flat = render(client, 6 * g_quarter);
		double difference = 0.0;
		for (size_t i = 0; i < flat.size(); ++i)
			difference = std::max(difference, static_cast<double>(std::abs(flat[i] - warped[i])));
		CHECK(difference < 1e-6);
		run(client, "project.set", {{"tempo", 120.0}});
		checkClicks(render(client, 6 * g_quarter), expected);
		// Undo plays the warped clip again.
		run(client, "history.undo");
		run(client, "history.undo");
		CHECK(eventOf(client, loop.event).at("clip") == loop.clip);
	}
}

TEST_CASE("a clip's warp is saved with the project", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const Loop loop = addLoop(client, dir, clicks({1000, 25000, 49000, 73000}, 4 * g_quarter));
	run(client, "clip.setWarp", {{"clip", loop.clip}, {"musical", true}, {"preset", "drums"}, {"gridStart", 1000}});
	run(client, "clip.adjustGrid", {{"clip", loop.clip}, {"mode", "beat"}, {"beat", 2}, {"frame", 50000}});
	run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 25000}, {"target", 26000}});
	const Json warp = warpOf(client, loop.clip);
	const std::string file = (dir.path() / "warp.dawproject").u8string();
	run(client, "project.save", {{"path", file}, {"embedMedia", false}});
	run(client, "project.new");
	run(client, "project.load", {{"path", file}});
	CHECK(warpOf(client, loop.clip) == warp);
	CHECK(warp.at("preset") == "drums");
	CHECK(warp.at("beats").size() == 1);
	CHECK(warp.at("markers").size() == 1);
}
