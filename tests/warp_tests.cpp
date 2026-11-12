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
#include <filesystem>
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

	// A steady 750 Hz sine at half scale, mono at 48 kHz.
	test::Audio sine(int64_t frames)
	{
		test::Audio audio;
		for (int64_t i = 0; i < frames; ++i)
			audio.samples.push_back(
				static_cast<float>(0.5 * std::sin(2.0 * g_pi * 750.0 * static_cast<double>(i) / 48000.0)));
		return audio;
	}

	// The RMS level of 1200 samples from a frame.
	double level(const std::vector<float>& samples, int64_t from)
	{
		double sum = 0.0;
		for (int64_t i = from; i < from + 1200; ++i)
			sum += static_cast<double>(samples[static_cast<size_t>(i)]) * samples[static_cast<size_t>(i)];
		return std::sqrt(sum / 1200.0);
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
	const std::vector<float> slow = render(client, 10 * g_quarter);
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

TEST_CASE("splitting and resizing a warped event leaves its audio where it plays", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 256}, {"inputChannels", 0}, {"captureSeconds", 6}});
	const std::vector<double> slow{2000.0, 50000.0, 98000.0, 146000.0};

	SECTION("musical mode")
	{
		// Four quarters at 120 BPM in musical mode, then the project goes to 60 BPM: each quarter takes twice as long.
		const Loop loop = addLoop(client, dir, clicks({1000, 25000, 49000, 73000}, 4 * g_quarter));
		run(client, "project.set", {{"tempo", 120.0}});
		run(client, "clip.setWarp", {{"clip", loop.clip}, {"musical", true}});
		run(client, "project.set", {{"tempo", 60.0}});
		checkClicks(render(client, 8 * g_quarter), slow);

		// A split at project beat 1 cuts at clip beat 1.
		const Json pieces =
			run(client, "event.split", {{"events", {loop.event}}, {"positions", {2 * g_quarter}}}).at("events");
		REQUIRE(pieces.size() == 1);
		const auto right = pieces[0].get<ObjectId>();
		CHECK(eventOf(client, loop.event).at("length") == g_quarter);
		CHECK(eventOf(client, right).at("offset") == g_quarter);
		CHECK(eventOf(client, right).at("length") == 3 * g_quarter);
		checkClicks(render(client, 8 * g_quarter), slow);
		CHECK(
			run(client, "event.split", {{"events", {right}}, {"positions", {4 * g_quarter}}}).at("events").size() == 1);
		run(client, "history.undo");

		// The start a beat later hides clip beat 1, the end a beat earlier clip beat 3.
		run(client, "event.resize", {{"events", {right}}, {"edge", "start"}, {"delta", 2 * g_quarter}});
		CHECK(eventOf(client, right).at("offset") == 2 * g_quarter);
		CHECK(eventOf(client, right).at("length") == 2 * g_quarter);
		run(client, "event.resize", {{"events", {right}}, {"edge", "end"}, {"delta", -2 * g_quarter}});
		CHECK(eventOf(client, right).at("length") == g_quarter);
		checkClicks(render(client, 8 * g_quarter), {2000.0, 98000.0});

		// The snap point at project beat 2 is clip beat 2.
		run(client, "event.setSnapPoint", {{"events", {right}}, {"position", 4 * g_quarter}});
		CHECK(eventOf(client, right).at("snapPoint") == 2 * g_quarter);
	}

	SECTION("free warp markers")
	{
		const Loop loop = addLoop(client, dir, clicks({1000, 25000, 52000, 73000}, 4 * g_quarter), "linear");
		run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 52000}, {"target", 49000}});
		const std::vector<double> warped{
			1000.0 * 49000.0 / 52000.0, 25000.0 * 49000.0 / 52000.0, 49000.0, 49000.0 + (21000.0 * 47000.0 / 44000.0)};
		run(client, "event.split", {{"events", {loop.event}}, {"positions", {30000}}});
		checkClicks(render(client, 4 * g_quarter), warped);
		// A snap point goes to the clip frame that plays at its position.
		run(client, "event.setSnapPoint", {{"events", {loop.event}}, {"position", 49000 - 30000}});
		CHECK(eventOf(client, loop.event).at("snapPoint") == std::llround((49000.0 - 30000.0) * 52000.0 / 49000.0));
	}

	SECTION("a start resized past the clip's start")
	{
		// Clip frame 60000 plays at 96000, so frame 0 plays where the warp puts it before 60000: at 38454.5.
		const Loop loop = addLoop(client, dir, clicks({1000, 25000, 52000, 73000}, 4 * g_quarter), "linear");
		run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 52000}, {"target", 49000}});
		run(client, "event.remove", {{"events", {loop.event}}});
		const auto event = run(client, "audioEvent.add",
			{{"track", loop.track}, {"clip", loop.clip}, {"position", 96000}, {"offset", 60000}, {"length", 36000}})
							   .at("event")
							   .at("id")
							   .get<ObjectId>();
		const double frame0 = 96000.0 - (49000.0 + (8000.0 * 47000.0 / 44000.0));
		checkClicks(render(client, 5 * g_quarter), {frame0 + 49000.0 + (21000.0 * 47000.0 / 44000.0)});
		run(client, "event.resize", {{"events", {event}}, {"edge", "start"}, {"delta", -96000}});
		CHECK(eventOf(client, event).at("offset") == 0);
		CHECK(std::abs(eventOf(client, event).at("position").get<double>() - frame0) <= 1.0);
		checkClicks(render(client, 5 * g_quarter),
			{frame0 + (1000.0 * 49000.0 / 52000.0), frame0 + (25000.0 * 49000.0 / 52000.0), frame0 + 49000.0,
				frame0 + 49000.0 + (21000.0 * 47000.0 / 44000.0)});
	}
}

TEST_CASE("bars and beats, Auto Adjust and Manual Adjust define a clip's grid", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Two bars at 100 BPM (28800 frames a quarter), the third quarter's click 1000 frames late.
	const int64_t beat = 28800;
	std::vector<int64_t> at;
	at.reserve(8);
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
	// From the first click to the end, two bars: the late click's beat is edited onto it, and the event's snap point
	// moves to the start of the section.
	CHECK_THAT(
		reject(client, "clip.autoAdjust", {{"clip", loop.clip}, {"event", 999}}), ContainsSubstring("no audio event"));
	warp = run(client, "clip.autoAdjust",
		{{"clip", loop.clip}, {"event", loop.event}, {"start", 380}, {"end", (8 * beat) + 380}})
			   .at("warp");
	CHECK(eventOf(client, loop.event).at("snapPoint") == 380);
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
		CHECK(std::abs(event.at("length").get<int64_t>() - (int64_t{4} * 36000)) <= 1); // four quarters at 80 BPM
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

// Follow-ups of #884 (#887, #890).
TEST_CASE("warp markers that stretch or squeeze beyond the real-time range are refused", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const Loop loop = addLoop(client, dir, clicks({1000, 25000, 52000, 73000}, 4 * g_quarter), "linear");
	// 1:8 both ways: the first 8000 frames eight times as long, or 64000 frames in 8000.
	CHECK_THAT(reject(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 8000}, {"target", 64000}}),
		ContainsSubstring("4 times"));
	CHECK_THAT(reject(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 64000}, {"target", 8000}}),
		ContainsSubstring("4 times"));
	// The segment up to the clip's end counts too: 6000 frames in 1000.
	CHECK_THAT(reject(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 90000}, {"target", 95000}}),
		ContainsSubstring("4 times"));
	// 1:4 is within it; moving the marker beyond is refused.
	run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 16000}, {"target", 64000}});
	CHECK_THAT(reject(client, "clip.setWarpMarker", {{"clip", loop.clip}, {"position", 16000}, {"target", 80000}}),
		ContainsSubstring("4 times"));
	CHECK(warpOf(client, loop.clip).at("markers") == Json::array({{{"position", 16000}, {"target", 64000}}}));
}

TEST_CASE("a warped event plays its clip fades where they play, also from inside them", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const Loop loop = addLoop(client, dir, sine(4 * g_quarter), "linear");
	run(client, "clip.setFades",
		{{"clip", loop.clip}, {"fadeIn", {{"length", 24000}, {"shape", "linear"}}},
			{"fadeOut", {{"length", 24000}, {"shape", "linear"}}}});
	// Clip frames 0 to 48000 play in 24000 samples, the rest one and a half times as long.
	run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 48000}, {"target", 24000}});
	// The event plays clip frames 12000 to 84000: it starts halfway into the fade in, which plays from 6000 samples
	// before its start to 6000 after it, and ends halfway into the fade out, from 54000 to 90000.
	run(client, "audioEvent.set", {{"event", loop.event}, {"offset", 12000}, {"length", 72000}});
	const std::vector<float> played = render(client, 72000);
	const double steady = level(played, 30000);
	REQUIRE(steady > 0.2);
	CHECK(std::abs((level(played, 600) / steady) - 0.6) < 0.08);
	CHECK(std::abs((level(played, 70200) / steady) - (19200.0 / 36000.0)) < 0.08);
}

TEST_CASE("flattening renders the clip fades and keeps the event fades as they played", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const Loop loop = addLoop(client, dir, sine(4 * g_quarter), "linear");
	run(client, "clip.setFades",
		{{"clip", loop.clip}, {"fadeIn", {{"length", 24000}, {"shape", "linear"}}},
			{"fadeOut", {{"length", 24000}, {"shape", "sCurve"}}}});
	run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 48000}, {"target", 24000}});
	run(client, "audioEvent.set", {{"event", loop.event}, {"offset", 12000}, {"length", 72000}});
	run(client, "audioEvent.setFades",
		{{"event", loop.event}, {"fadeIn", {{"length", 4800}}}, {"fadeOut", {{"length", 9600}}}});
	const std::vector<float> warped = render(client, 80000);
	run(client, "event.flatten", {{"events", {loop.event}}});
	const Json event = eventOf(client, loop.event);
	CHECK(event.at("fadeIn").at("length") == 4800);
	CHECK(event.at("fadeOut").at("length") == 9600);
	const std::vector<float> flat = render(client, 80000);
	double difference = 0.0;
	for (size_t i = 0; i < flat.size(); ++i)
		difference = std::max(difference, static_cast<double>(std::abs(flat[i] - warped[i])));
	CHECK(difference < 1e-5);
}

TEST_CASE("a failed flatten leaves no files behind", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const Loop loop = addLoop(client, dir, clicks({1000, 25000, 49000, 73000}, 4 * g_quarter));
	run(client, "clip.setWarp", {{"clip", loop.clip}, {"musical", true}});
	// A second warped clip whose file is gone: flattening its event fails after the first one's file is written.
	const std::string other = (dir.path() / "other.wav").u8string();
	test::writeWav(other, clicks({1000}, g_quarter));
	const Json media = run(client, "media.add", {{"path", other}}).at("media");
	const auto clip = run(client, "clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<ObjectId>();
	run(client, "clip.setWarp", {{"clip", clip}, {"musical", true}});
	const auto second = run(client, "audioEvent.add",
		{{"track", loop.track}, {"clip", clip}, {"position", 8 * g_quarter}, {"offset", 0}, {"length", g_quarter}})
							.at("event")
							.at("id")
							.get<ObjectId>();
	const std::string file = (dir.path() / "flatten.dawproject").u8string();
	run(client, "project.save", {{"path", file}, {"embedMedia", false}});
	std::filesystem::remove(std::filesystem::u8path(other));
	reject(client, "event.flatten", {{"events", {loop.event, second}}});
	for (const auto& entry : std::filesystem::recursive_directory_iterator(dir.path()))
		CHECK(entry.path().filename().u8string().find("Flattened") == std::string::npos);
	CHECK(eventOf(client, loop.event).at("clip") == loop.clip);
}

TEST_CASE("timeline positions within a warped event go through its warp", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Clip frame 52000 plays at 49000.
	const Loop loop = addLoop(client, dir, clicks({1000, 25000, 52000, 73000}, 4 * g_quarter), "linear");
	run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 52000}, {"target", 49000}});

	// Fade In to Range Start, and Fade Out from Range End.
	run(client, "range.fades", {{"start", 0}, {"end", 49000}, {"tracks", {loop.track}}, {"fade", "in"}});
	// The event's own fades play from its edges in frames at the clip's sample rate (where they play: below).
	CHECK(eventOf(client, loop.event).at("fadeIn").at("length") == 49000);
	run(client, "range.fades", {{"start", 49000}, {"end", 96000}, {"tracks", {loop.track}}, {"fade", "out"}});
	CHECK(eventOf(client, loop.event).at("fadeOut").at("length") == (4 * g_quarter) - 49000);

	// A volume curve drawn from 49000 starts at clip frame 52000.
	run(client, "range.volumeCurve", {{"start", 49000}, {"end", 70000}, {"tracks", {loop.track}}, {"volume", -6.0}});
	bool found = false;
	const Json curve = eventOf(client, loop.event).at("volumeCurve");
	for (const Json& point : curve)
		found = found || (point.at("position") == 52000 && point.at("volume") == -6.0);
	CHECK(found);

	// Cut at the hitpoint at clip frame 52000: the piece after it starts at 49000.
	run(client, "clip.setHitpoints",
		{{"clip", loop.clip}, {"hitpoints", {{{"position", 52000}, {"peak", -2.0}, {"intensity", 1.0}}}}});
	const Json events = run(client, "event.fromHitpoints", {{"event", loop.event}, {"create", "events"}}).at("events");
	REQUIRE(events.size() == 2);
	const Json piece = eventOf(client, events.at(1).get<ObjectId>());
	CHECK(piece.at("offset") == 52000);
	CHECK(std::abs(piece.at("position").get<int64_t>() - 49000) <= 1);
}

TEST_CASE("fades to a range on a warped event play to the range", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Clip frame 52000 plays at 49000.
	const Loop loop = addLoop(client, dir, sine(4 * g_quarter), "linear");
	run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 52000}, {"target", 49000}});

	// Fade In to Range Start over 0 to 49000: full gain at 49000, half of it halfway there.
	run(client, "range.fades", {{"start", 0}, {"end", 49000}, {"tracks", {loop.track}}, {"fade", "in"}});
	std::vector<float> played = render(client, 4 * g_quarter);
	double steady = level(played, 70000);
	REQUIRE(steady > 0.2);
	CHECK(std::abs((level(played, 23900) / steady) - 0.5) < 0.015);
	CHECK(level(played, 49000) / steady > 0.98);

	// Fade Out from Range End at 49000: it starts falling there, half of it halfway to the event's end at 96000.
	run(client, "audioEvent.setFades", {{"event", loop.event}, {"fadeIn", {{"length", 0}}}});
	run(client, "range.fades", {{"start", 49000}, {"end", 96000}, {"tracks", {loop.track}}, {"fade", "out"}});
	played = render(client, 4 * g_quarter);
	steady = level(played, 20000);
	REQUIRE(steady > 0.2);
	CHECK(level(played, 47800) / steady > 0.98);
	CHECK(std::abs((level(played, 71900) / steady) - 0.5) < 0.015);
}

TEST_CASE("an event's fades fit into what a stretched warped event plays", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Four quarters at 120 BPM in musical mode, then the project goes to 60 BPM: the event plays twice its length.
	const Loop loop = addLoop(client, dir, sine(4 * g_quarter));
	run(client, "project.set", {{"tempo", 120.0}});
	run(client, "clip.setWarp", {{"clip", loop.clip}, {"musical", true}});
	run(client, "project.set", {{"tempo", 60.0}});

	// Adjust Fades to Range inside it: both fades together take more than the event's length.
	run(client, "range.fades", {{"start", 90000}, {"end", 100000}, {"tracks", {loop.track}}, {"fade", "adjust"}});
	CHECK(eventOf(client, loop.event).at("fadeIn").at("length") == 90000);
	CHECK(eventOf(client, loop.event).at("fadeOut").at("length") == (8 * g_quarter) - 100000);

	// audioEvent.setFades checks them against the same length: a shape change keeps both.
	run(client, "audioEvent.setFades", {{"event", loop.event}, {"fadeIn", {{"shape", "sCurve"}}}});
	CHECK(eventOf(client, loop.event).at("fadeIn").at("shape") == "sCurve");
	CHECK(eventOf(client, loop.event).at("fadeIn").at("length") == 90000);
	CHECK_THAT(reject(client, "audioEvent.setFades", {{"event", loop.event}, {"fadeIn", {{"length", 100001}}}}),
		ContainsSubstring("192000 sample frames"));

	// Back at 120 BPM it plays its 96000 frames, less than the fades: they stay, a shape change and a shorter fade
	// still go, a longer one doesn't.
	run(client, "project.set", {{"tempo", 120.0}});
	run(client, "audioEvent.setFades", {{"event", loop.event}, {"fadeOut", {{"shape", "sCurve"}}}});
	CHECK(eventOf(client, loop.event).at("fadeOut").at("shape") == "sCurve");
	run(client, "audioEvent.setFades", {{"event", loop.event}, {"fadeIn", {{"length", 50000}}}});
	CHECK(eventOf(client, loop.event).at("fadeIn").at("length") == 50000);
	CHECK_THAT(reject(client, "audioEvent.setFades", {{"event", loop.event}, {"fadeIn", {{"length", 50001}}}}),
		ContainsSubstring("96000 sample frames"));
}

TEST_CASE("warp quantizing keeps the markers within the real-time range", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// The marker at 1000 plays nearest to the quarter at 0, but the 1000 frames before it can't play in less than
	// 250: it goes as near as it can.
	const Loop loop = addLoop(client, dir, clicks({1000, 25000, 52000, 73000}, 4 * g_quarter), "linear");
	run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 1000}});
	run(client, "quantize.set", {{"preset", "1/4"}, {"settings", {{"warpQuantize", true}}}});
	run(client, "quantize.apply", {{"events", {loop.event}}});
	CHECK(warpOf(client, loop.clip).at("markers") == Json::array({{{"position", 1000}, {"target", 250}}}));
}

TEST_CASE("Crossfade closes a gap and overlaps through the warp of warped events", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Clip frame f before 52000 plays at f * 49 / 52. Two events of the clip: 0 to 20000 and, at 40000, 30000 on.
	const Loop loop = addLoop(client, dir, clicks({1000, 25000, 52000, 73000}, 4 * g_quarter), "linear");
	run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 52000}, {"target", 49000}});
	const std::vector<engine::dsp::WarpPoint> map{{0.0, 0.0}, {52000.0, 49000.0}, {96000.0, 96000.0}};
	const auto plays = [&map](double frame) { return engine::dsp::warpTarget(map, frame); };
	run(client, "audioEvent.set", {{"event", loop.event}, {"length", 20000}});
	const auto later = run(client, "audioEvent.add",
		{{"track", loop.track}, {"clip", loop.clip}, {"position", 40000}, {"offset", 30000}, {"length", 30000}})
						   .at("event")
						   .at("id")
						   .get<ObjectId>();

	run(client, "quantize.crossfade", {{"events", {loop.event, later}}, {"length", 10.0}});
	// The later event starts where the first one ended, its audio where it played: clip frame 30000 still at 40000.
	const Json second = eventOf(client, later);
	const auto offset = second.at("offset").get<double>();
	const auto start = second.at("position").get<double>();
	CHECK(std::abs(start - plays(20000.0)) <= 1.0);
	CHECK(std::abs(start + plays(30000.0) - plays(offset) - 40000.0) <= 1.5);
	// The first one goes on 10 ms (480 samples) into it, with a crossfade.
	const Json first = eventOf(client, loop.event);
	CHECK(std::abs(plays(first.at("length").get<double>()) - (start + 480.0)) <= 1.5);
	CHECK(second.at("crossfade").is_object());
}

TEST_CASE("snapping and Detect Silence in a warped event go through its warp", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Clip frame f before 52000 plays at f * 49 / 52. Silence but for a sine from 24000 to 72000, which crosses zero
	// at every 32nd frame.
	test::Audio audio = sine(4 * g_quarter);
	for (size_t i = 0; i < audio.samples.size(); ++i)
		if (i < 24000 || i >= 72000)
			audio.samples[i] = 0.0F;
	const Loop loop = addLoop(client, dir, audio, "linear");
	run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 52000}, {"target", 49000}});

	SECTION("zero crossings")
	{
		// 24510 plays clip frame 26010.6; the crossing nearest to it is at 26016, which plays at 24515.
		const Json split =
			run(client, "event.split", {{"events", {loop.event}}, {"positions", {24510}}, {"zeroCrossing", true}});
		REQUIRE(split.at("events").size() == 1);
		const Json piece = eventOf(client, split.at("events").at(0).get<ObjectId>());
		CHECK(piece.at("offset") == 26016);
		CHECK(std::abs(piece.at("position").get<int64_t>() - 24515) <= 1);
	}
	SECTION("the snap point")
	{
		// 24500 plays clip frame 26000, and snapping finds it there.
		run(client, "event.setSnapPoint", {{"events", {loop.event}}, {"position", 24500}});
		CHECK(eventOf(client, loop.event).at("snapPoint") == 26000);
		run(client, "snap.set", {{"on", true}, {"type", "events"}});
		CHECK(run(client, "snap.position", {{"position", 24600}}).at("position") == 24500);
	}
	SECTION("Detect Silence's splits")
	{
		// The event that remains plays its audio where it played.
		const Json kept =
			run(client, "event.stripSilence", {{"events", {loop.event}}, {"minimumClosed", 0}}).at("events");
		REQUIRE(kept.size() == 1);
		const Json piece = eventOf(client, kept.at(0).get<ObjectId>());
		const std::vector<engine::dsp::WarpPoint> map{{0.0, 0.0}, {52000.0, 49000.0}, {96000.0, 96000.0}};
		CHECK(std::abs(piece.at("offset").get<int64_t>() - 24000) <= 48);
		CHECK(std::abs(piece.at("position").get<double>() -
				  engine::dsp::warpTarget(map, piece.at("offset").get<double>())) <= 1.0);
	}
}

TEST_CASE("the sample editor selects the locators through the warp", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Clip frame 52000 plays at 49000, and 26000 at 24500.
	const Loop loop = addLoop(client, dir, clicks({1000, 25000, 52000, 73000}, 4 * g_quarter), "linear");
	run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 52000}, {"target", 49000}});
	run(client, "sampleEditor.open", {{"event", loop.event}});
	run(client, "transport.set", {{"left", 24500}, {"right", 49000}});
	const Json view = run(client, "sampleEditor.set", {{"select", "loop"}});
	CHECK(view.at("selectionStart") == 26000);
	CHECK(view.at("selectionEnd") == 52000);
}

TEST_CASE("Apply Fades of Strip Silence fit into what a squeezed warped event plays", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// Four quarters at 120 BPM in musical mode, then the project goes to 240 BPM: the event plays half its length.
	const Loop loop = addLoop(client, dir, sine(4 * g_quarter));
	run(client, "project.set", {{"tempo", 120.0}});
	run(client, "clip.setWarp", {{"clip", loop.clip}, {"musical", true}});
	run(client, "project.set", {{"tempo", 240.0}});

	// A second each asks for 48000 frames, half the event's length: each gets half of the 48000 it plays.
	run(client, "event.stripSilence", {{"events", {loop.event}}, {"strip", false}, {"fades", 1000}});
	const Json event = eventOf(client, loop.event);
	CHECK(event.at("fadeIn").at("length") == 24000);
	CHECK(event.at("fadeOut").at("length") == 24000);
}

TEST_CASE("resizing a warped event with its contents keeps the edge that stays put", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const Loop loop = addLoop(client, dir, clicks({1000, 25000, 52000, 73000}, 4 * g_quarter), "linear");
	run(client, "clip.addWarpMarker", {{"clip", loop.clip}, {"position", 52000}, {"target", 49000}});
	// Where the event ends: clip frames play where the marker puts them, from the event's position.
	const std::vector<engine::dsp::WarpPoint> map{{0.0, 0.0}, {52000.0, 49000.0}, {96000.0, 96000.0}};
	const auto end = [&client, &loop, &map]
	{
		const Json event = eventOf(client, loop.event);
		const auto offset = event.at("offset").get<double>();
		return event.at("position").get<double>() +
			engine::dsp::warpTarget(map, offset + event.at("length").get<double>()) -
			engine::dsp::warpTarget(map, offset);
	};

	SECTION("the end")
	{
		// The clip's last frame stays at the end, now at 76000.
		run(client, "event.resize",
			{{"events", {loop.event}}, {"edge", "end"}, {"delta", -20000}, {"mode", "moveContents"}});
		const Json event = eventOf(client, loop.event);
		CHECK(event.at("position") == 0);
		CHECK(event.at("offset").get<int64_t>() + event.at("length").get<int64_t>() == 4 * g_quarter);
		CHECK(std::abs(end() - 76000.0) <= 1.5);
	}
	SECTION("the start")
	{
		// The start goes to 20000 with the clip's first frame; the end stays at 96000.
		run(client, "event.resize",
			{{"events", {loop.event}}, {"edge", "start"}, {"delta", 20000}, {"mode", "moveContents"}});
		const Json event = eventOf(client, loop.event);
		CHECK(event.at("position") == 20000);
		CHECK(event.at("offset") == 0);
		CHECK(std::abs(end() - 96000.0) <= 1.5);
	}
}

TEST_CASE("the warped events' own media entries are no media files", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const Loop loop = addLoop(client, dir, clicks({1000, 25000, 49000, 73000}, 4 * g_quarter));
	run(client, "clip.setWarp", {{"clip", loop.clip}, {"musical", true}});
	reject(client, "media.peaks", {{"media", loop.event}, {"pixels", 10}});
	run(client, "audio.setDevice", {{"blockSize", 256}, {"inputChannels", 0}});
	CHECK_THAT(
		reject(client, "audition.start", {{"media", loop.event}, {"position", 0}}), ContainsSubstring("no media file"));
}
