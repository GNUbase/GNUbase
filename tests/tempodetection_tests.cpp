// SPDX-License-Identifier: GPL-3.0-or-later
// Tempo detection and tempo from MIDI (#158, docs/features/tempo-detection.md): beat tracking on pulses whose beats are
// known, tempo.detect on a synthesized drum loop and on MIDI parts, and tempo.fromMidi on tapped notes.
#include "engine/analysis/tempodetect.h"
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;
using time::g_ticksPerQuarter;

namespace
{
	constexpr double g_pi = 3.14159265358979323846;

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

	// The beats of a tempo going from one BPM to another over beats beats, linearly per beat, from start seconds.
	std::vector<double> beatsOf(double start, double from, double to, int beats)
	{
		std::vector<double> times{start};
		for (int i = 0; i + 1 < beats; ++i)
			times.push_back(times.back() + (60.0 / (from + ((to - from) * i / std::max(1, beats - 2)))));
		return times;
	}

	// Checks that every detected beat is within tolerance seconds of a true one, and that they cover the true ones
	// from the first to the last but one.
	void checkBeats(const std::vector<double>& detected, const std::vector<double>& truth, double tolerance)
	{
		REQUIRE(detected.size() >= 2);
		for (const double beat : detected)
		{
			const auto near =
				std::find_if(truth.begin(), truth.end(), [&](double t) { return std::abs(t - beat) <= tolerance; });
			INFO("detected beat at " << beat);
			CHECK(near != truth.end());
		}
		CHECK(detected.size() + 1 >= truth.size());
	}

	// The tempo track's events as a tempo map.
	time::TempoMap tempoMap(const Json& tempo)
	{
		std::vector<time::TempoPoint> points;
		for (const Json& point : tempo.at("tempoTrack"))
			points.push_back(
				{point.at("position").get<int64_t>(), point.at("bpm").get<double>(), point.at("ramp").get<bool>()});
		return time::TempoMap(points);
	}

	// A drum loop with a kick on each of the beats (seconds) and a quieter hi-hat between each two, 48 kHz mono.
	test::Audio drumLoop(const std::vector<double>& beats, double seconds)
	{
		test::Audio audio;
		audio.samples.assign(static_cast<size_t>(seconds * audio.sampleRate), 0.0F);
		uint32_t noise = 12345;
		std::vector<std::pair<double, bool>> hits; // when, and whether it is a kick
		for (size_t i = 0; i < beats.size(); ++i)
		{
			hits.emplace_back(beats[i], true);
			if (i + 1 < beats.size())
				hits.emplace_back((beats[i] + beats[i + 1]) / 2.0, false);
		}
		for (const auto& [at, kick] : hits)
		{
			const auto first = static_cast<size_t>(std::llround(at * audio.sampleRate));
			double phase = 0.0;
			for (size_t n = 0; n < static_cast<size_t>(0.2 * audio.sampleRate); ++n)
			{
				if (first + n >= audio.samples.size())
					break;
				const double t = static_cast<double>(n) / audio.sampleRate;
				double value = 0.0;
				if (kick)
				{
					phase += 2.0 * g_pi * (50.0 + (100.0 * std::exp(-t / 0.03))) / audio.sampleRate;
					value = 0.9 * std::sin(phase) * std::exp(-t / 0.1);
				}
				else
				{
					noise = (noise * 1664525U) + 1013904223U;
					value = 0.25 * ((static_cast<double>(noise >> 8U) / 8388608.0) - 1.0) * std::exp(-t / 0.015);
				}
				audio.samples[first + n] += static_cast<float>(value);
			}
		}
		return audio;
	}

	// The loop on a new linear audio track at the project start, as one event; returns the event.
	int64_t addLoop(engine::Client& client, const test::TempDir& dir, const test::Audio& loop)
	{
		const std::string file = (dir.path() / "loop.wav").u8string();
		test::writeWav(file, loop);
		const engine::ObjectId track = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}})
										   .at("track")
										   .at("id")
										   .get<engine::ObjectId>();
		const Json media = run(client, "media.add",
			{{"path", file}, {"sampleRate", 48000}, {"channels", 1}, {"length", static_cast<int64_t>(loop.frames())}})
							   .at("media");
		const auto clip = run(client, "clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
		return run(client, "audioEvent.add",
			{{"track", track}, {"clip", clip}, {"position", 0}, {"offset", 0},
				{"length", static_cast<int64_t>(loop.frames())}})
			.at("event")
			.at("id")
			.get<int64_t>();
	}

	struct Part
	{
		engine::ObjectId track = 0;
		engine::ObjectId part = 0;
	};

	// A part on a new linear MIDI track from sample 0, seconds long, with a short note at each of the times. At the
	// project's 120 BPM, a second is two quarters.
	Part addNotes(engine::Client& client, const std::vector<double>& times, double seconds)
	{
		Part made;
		made.track = run(client, "track.add", {{"type", "midi"}, {"timeBase", "linear"}})
						 .at("track")
						 .at("id")
						 .get<engine::ObjectId>();
		made.part = run(client, "midiPart.add",
			{{"track", made.track}, {"position", 0}, {"length", std::llround(seconds * 48000.0)}})
						.at("part")
						.at("id")
						.get<engine::ObjectId>();
		Json notes = Json::array();
		for (const double at : times)
			notes.push_back({{"position", std::llround(at * 2.0 * static_cast<double>(g_ticksPerQuarter))},
				{"length", g_ticksPerQuarter / 8}, {"pitch", 60}, {"velocity", 0.8}});
		run(client, "note.add", {{"part", made.part}, {"notes", notes}});
		return made;
	}

	// Where the part's notes start, in seconds, as the render snapshot plays them.
	std::vector<double> noteTimes(engine::Engine& engine, engine::ObjectId track)
	{
		const engine::RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
		const auto channel = std::find_if(snapshot.channels.begin(), snapshot.channels.end(),
			[track](const engine::RenderChannel& each) { return each.track == track; });
		REQUIRE(channel != snapshot.channels.end());
		std::vector<double> times;
		for (const auto& note : channel->notes)
			times.push_back(static_cast<double>(note.start) / 48000.0);
		std::sort(times.begin(), times.end());
		return times;
	}
}

TEST_CASE("beat tracking finds the beats of steady, drifting and wavering pulses", "[unit]")
{
	using engine::analysis::Pulse;

	SECTION("a steady 97 BPM with quieter offbeats")
	{
		const std::vector<double> truth = beatsOf(0.3, 97.0, 97.0, 32);
		std::vector<Pulse> pulses;
		for (size_t i = 0; i < truth.size(); ++i)
		{
			pulses.push_back({truth[i], 1.0});
			if (i + 1 < truth.size())
				pulses.push_back({(truth[i] + truth[i + 1]) / 2.0, 0.4});
		}
		const std::vector<double> beats = engine::analysis::detectBeats(pulses, truth.back() + 0.5);
		checkBeats(beats, truth, 1e-9); // on the pulses
	}
	SECTION("a tempo drifting from 100 to 125 BPM")
	{
		const std::vector<double> truth = beatsOf(0.0, 100.0, 125.0, 40);
		std::vector<Pulse> pulses;
		pulses.reserve(truth.size());
		for (const double beat : truth)
			pulses.push_back({beat, 1.0});
		checkBeats(engine::analysis::detectBeats(pulses, truth.back() + 0.2), truth, 1e-9);
	}
	SECTION("120 BPM played loosely, with rests")
	{
		std::vector<double> truth = beatsOf(1.0, 120.0, 120.0, 24);
		std::vector<Pulse> pulses;
		uint32_t state = 99;
		for (size_t i = 0; i < truth.size(); ++i)
		{
			state = (state * 1664525U) + 1013904223U;
			truth[i] += ((static_cast<double>(state >> 8U) / 16777216.0) - 0.5) * 0.016; // up to 8 ms off
			if (i % 8 != 5)
				pulses.push_back({truth[i], 1.0});
		}
		checkBeats(engine::analysis::detectBeats(pulses, truth.back() + 0.2), truth, 0.02);
	}
	SECTION("nothing to track")
	{
		CHECK(engine::analysis::detectBeats({}, 10.0).empty());
		CHECK(engine::analysis::detectBeats({{1.0, 1.0}}, 10.0).size() < 2);
	}
}

TEST_CASE("detected beats are rescaled, shifted by half a beat and smoothed", "[unit]")
{
	using engine::analysis::rescaleBeats;
	const std::vector<double> beats{0.0, 1.0, 2.0, 3.0};
	CHECK(rescaleBeats(beats, 1.0, false) == beats);
	CHECK(rescaleBeats(beats, 2.0, false) == std::vector<double>{0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0});
	CHECK(rescaleBeats(beats, 0.5, false) == std::vector<double>{0.0, 2.0});
	CHECK(rescaleBeats(beats, 4.0 / 3.0, false) == std::vector<double>{0.0, 0.75, 1.5, 2.25, 3.0});
	CHECK(rescaleBeats(beats, 1.0, true) == std::vector<double>{0.5, 1.5, 2.5});
	const std::vector<double> smoothed = engine::analysis::smoothBeats({0.1, 0.9, 1.9, 3.1});
	REQUIRE(smoothed.size() == 4);
	for (size_t i = 0; i < smoothed.size(); ++i)
		CHECK(std::abs(smoothed[i] - static_cast<double>(i)) < 1e-9);
}

TEST_CASE("tempo detection makes the tempo track follow a drum loop", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const double bpm = 104.0;
	// A musical MIDI track, which the detection switches to linear, with a part at the fourth quarter.
	const engine::ObjectId midi =
		run(client, "track.add", {{"type", "midi"}}).at("track").at("id").get<engine::ObjectId>();
	run(client, "midiPart.add", {{"track", midi}, {"position", 3 * g_ticksPerQuarter}, {"length", g_ticksPerQuarter}});
	const int64_t event = addLoop(client, dir, drumLoop(beatsOf(0.0, bpm, bpm, 21), 12.0));
	const int64_t partStart = int64_t{3} * 24000; // the fourth quarter at 120 BPM, in samples

	Json result = run(client, "tempo.detect", {{"event", event}});
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Detect tempo");
	const Json tempo = result.at("tempo");
	CHECK(tempo.at("tempoTrackActive") == true);
	const Json& points = tempo.at("tempoTrack");
	CHECK(points.size() >= 15);
	for (const Json& point : points)
	{
		INFO(point.dump());
		CHECK(std::abs(point.at("bpm").get<double>() - bpm) < 0.5);
	}
	// The kicks are on the beats.
	const time::TempoMap map = tempoMap(tempo);
	for (int beat = 0; beat < 18; ++beat)
	{
		INFO("beat " << beat);
		CHECK(std::abs(map.toSeconds(beat * g_ticksPerQuarter) - (beat * 60.0 / bpm)) < 0.005);
	}
	// Every track is linear now, so the MIDI part stays where it was.
	const Json project = run(client, "project.get");
	for (const Json& each : project.at("tracks"))
		if (each.at("id") == midi)
			CHECK(each.at("timeBase") == "linear");
	CHECK(project.at("midiParts").at(0).at("position") == partStart);

	SECTION("twice, half, 4/3 and 3/4 as fast")
	{
		for (const double factor : {2.0, 0.5, 4.0 / 3.0, 0.75})
		{
			INFO("factor " << factor);
			const Json detected = run(client, "tempo.detect", {{"event", event}, {"factor", factor}});
			const Json first = detected.at("tempo").at("tempoTrack").at(0);
			CHECK(std::abs(first.at("bpm").get<double>() - (bpm * factor)) < 0.5 * factor);
		}
		// Detecting again replaces what the detections before made.
		const Json again = run(client, "tempo.detect", {{"event", event}});
		for (const Json& point : again.at("tempo").at("tempoTrack"))
			CHECK(std::abs(point.at("bpm").get<double>() - bpm) < 0.5);
	}
	SECTION("offbeat correction puts the hi-hats on the beats")
	{
		const Json detected = run(client, "tempo.detect", {{"event", event}, {"offbeat", true}});
		const time::TempoMap shifted = tempoMap(detected.at("tempo"));
		const double hat = 30.0 / bpm; // the first hi-hat
		const time::Ticks at = shifted.toTicks(hat);
		CHECK(std::abs(shifted.toSeconds(at + (4 * g_ticksPerQuarter)) - (hat + (4 * 60.0 / bpm))) < 0.005);
	}
	SECTION("smoothing makes it one steady tempo")
	{
		const Json detected = run(client, "tempo.detect", {{"event", event}, {"smooth", true}});
		double slowest = 999.0;
		double fastest = 1.0;
		for (const Json& point : detected.at("tempo").at("tempoTrack"))
		{
			slowest = std::min(slowest, point.at("bpm").get<double>());
			fastest = std::max(fastest, point.at("bpm").get<double>());
		}
		CHECK(fastest - slowest < 0.01);
		CHECK(std::abs(fastest - bpm) < 0.1);
	}
	SECTION("reset removes the tempo events inside the event, undo the detection")
	{
		result = run(client, "tempo.detect", {{"event", event}, {"reset", true}});
		CHECK(result.at("tempo").at("tempoTrack").size() == 1);
		run(client, "history.undo");
		run(client, "history.undo");
		const Json undone = run(client, "project.get");
		CHECK(undone.at("tempoTrackActive") == false);
		CHECK(undone.at("tempoTrack").size() == 1);
	}
}

TEST_CASE("tempo detection keeps musical content after old tempo events where it plays", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// 120 BPM with a step to 90 BPM at bar 3, inside the loop, and a musical MIDI part at bar 9: 4 seconds to bar 3,
	// then 24 quarters at 90 BPM, so the part starts at 20 seconds.
	run(client, "tempo.add", {{"position", 8 * g_ticksPerQuarter}, {"bpm", 90.0}, {"ramp", false}});
	run(client, "project.set", {{"tempoTrackActive", true}});
	const engine::ObjectId midi =
		run(client, "track.add", {{"type", "midi"}}).at("track").at("id").get<engine::ObjectId>();
	run(client, "midiPart.add", {{"track", midi}, {"position", 32 * g_ticksPerQuarter}, {"length", g_ticksPerQuarter}});
	const int64_t event = addLoop(client, dir, drumLoop(beatsOf(0.0, 104.0, 104.0, 21), 12.0));

	run(client, "tempo.detect", {{"event", event}});
	const Json part = run(client, "project.get").at("midiParts").at(0);
	CHECK(part.at("position") == int64_t{20} * 48000);
}

TEST_CASE("tempo detection follows a drum loop that speeds up", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	// From 92 to 108 BPM over 24 beats, after half a second of silence.
	const std::vector<double> truth = beatsOf(0.5, 92.0, 108.0, 24);
	const int64_t event = addLoop(client, dir, drumLoop(truth, truth.back() + 1.0));
	const Json result = run(client, "tempo.detect", {{"event", event}});
	CHECK(result.at("beats") == truth.size());
	const time::TempoMap map = tempoMap(result.at("tempo"));
	const time::Ticks first = map.toTicks(truth.front());
	for (size_t i = 0; i < truth.size(); ++i)
	{
		INFO("beat " << i);
		CHECK(std::abs(map.toSeconds(first + (static_cast<time::Ticks>(i) * g_ticksPerQuarter)) - truth[i]) < 0.005);
	}
	CHECK(std::abs(map.bpmAt(first) - 92.0) < 1.0);
	CHECK(std::abs(map.bpmAt(first + (22 * g_ticksPerQuarter)) - 108.0) < 1.0);
}

TEST_CASE("tempo detection works on MIDI parts and needs seven seconds of material", "[unit]")
{
	engine::Engine engine;
	engine::Client client(engine);
	const std::vector<double> truth = beatsOf(0.0, 90.0, 90.0, 16);
	const Part made = addNotes(client, truth, 10.5);
	const Json tempo = run(client, "tempo.detect", {{"event", made.part}}).at("tempo");
	for (const Json& point : tempo.at("tempoTrack"))
		CHECK(std::abs(point.at("bpm").get<double>() - 90.0) < 0.05);

	const Part shorter = addNotes(client, beatsOf(0.0, 90.0, 90.0, 8), 6.0);
	CHECK_THAT(reject(client, "tempo.detect", {{"event", shorter.part}}), ContainsSubstring("7 seconds"));
	CHECK_THAT(reject(client, "tempo.detect", {{"event", 9999}}), ContainsSubstring("no audio event or MIDI part"));
	CHECK_THAT(reject(client, "tempo.detect", {{"event", made.part}, {"factor", 0}}), ContainsSubstring("factor"));
}

TEST_CASE("the tempo is calculated from tapped MIDI notes", "[unit]")
{
	engine::Engine engine;
	engine::Client client(engine);
	// Quarters tapped from 100 to 140 BPM, the first at 1.15 seconds: 2.3 quarters at 120 BPM.
	const std::vector<double> taps = beatsOf(1.15, 100.0, 140.0, 12);
	const Part made = addNotes(client, taps, 8.0);
	CHECK(noteTimes(engine, made.track).size() == taps.size());

	SECTION("a quarter apart")
	{
		const Json tempo = run(client, "tempo.fromMidi", {{"parts", {made.part}}}).at("tempo");
		CHECK(run(client, "history.get").at("steps").back().at("action") == "Calculate tempo from MIDI");
		CHECK(tempo.at("tempoTrackActive") == true);
		const time::TempoMap map = tempoMap(tempo);
		const time::Ticks first = map.toTicks(taps.front());
		// Each note is a quarter after the one before, and keeps its time.
		for (size_t i = 0; i < taps.size(); ++i)
		{
			INFO("note " << i);
			CHECK(
				std::abs(map.toSeconds(first + (static_cast<time::Ticks>(i) * g_ticksPerQuarter)) - taps[i]) < 0.0001);
		}
		const std::vector<double> times = noteTimes(engine, made.track);
		for (size_t i = 0; i < taps.size(); ++i)
			CHECK(std::abs(times[i] - taps[i]) < 0.0001);
		CHECK(std::abs(map.bpmAt(first) - 100.0) < 0.01);
	}
	SECTION("an eighth apart, from the selected notes")
	{
		const Json part = run(client, "project.get").at("midiParts").at(0);
		std::vector<engine::ObjectId> notes;
		for (const Json& note : part.at("notes"))
			notes.push_back(note.at("id").get<engine::ObjectId>());
		std::sort(notes.begin(), notes.end());
		notes.resize(4);
		const Json tempo = run(
			client, "tempo.fromMidi", {{"parts", {made.part}}, {"notes", notes}, {"distance", g_ticksPerQuarter / 2}})
							   .at("tempo");
		const time::TempoMap map = tempoMap(tempo);
		CHECK(std::abs(map.bpmAt(map.toTicks(taps.front())) - 50.0) < 0.01);
		CHECK(tempo.at("tempoTrack").size() == 4); // the first and one for each note but the last
	}
	SECTION("beginning at a bar start")
	{
		const Json tempo =
			run(client, "tempo.fromMidi", {{"parts", {made.part}}, {"beginAtBarStart", true}}).at("tempo");
		const time::TempoMap map = tempoMap(tempo);
		// 2.3 quarters is nearer bar 2 than bar 1: the first note goes to bar 2.
		CHECK(std::abs(map.toSeconds(4 * g_ticksPerQuarter) - taps.front()) < 0.0001);
		CHECK(std::abs(map.toSeconds(5 * g_ticksPerQuarter) - taps[1]) < 0.0001);
		const std::vector<double> times = noteTimes(engine, made.track);
		CHECK(std::abs(times.front() - taps.front()) < 0.0001);
	}
	SECTION("errors")
	{
		CHECK_THAT(
			reject(client, "tempo.fromMidi", {{"parts", {made.part}}, {"distance", 0}}), ContainsSubstring("distance"));
		const Json part = run(client, "project.get").at("midiParts").at(0);
		const auto note = part.at("notes").at(0).at("id").get<engine::ObjectId>();
		CHECK_THAT(
			reject(client, "tempo.fromMidi", {{"parts", {made.part}}, {"notes", {note}}}), ContainsSubstring("two"));
	}
}
