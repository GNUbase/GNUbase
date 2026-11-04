// SPDX-License-Identifier: GPL-3.0-or-later
// Hitpoints and slices (#143, docs/features/hitpoints.md): transient detection on drum loops whose hits are known,
// the hitpoints of clips with their filters and edits, and what is made of them. tests/hitpoints_ui_tests.cpp drives
// the sample editor's Hitpoints section in the app.
#include "engine/analysis/onsets.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
	constexpr double g_pi = 3.14159265358979323846;
	constexpr size_t g_ghost = 9; // the rock loop's ghost note among its hits

	enum class Drum : uint8_t
	{
		Kick,
		Snare,
		Hat,
		OpenHat,
	};

	// White noise in -1 to 1, the same on every platform.
	struct Noise
	{
		uint32_t state = 12345;

		double next()
		{
			state = (state * 1664525U) + 1013904223U;
			return (static_cast<double>(state >> 8U) / 8388608.0) - 1.0;
		}
	};

	// A drum loop and where its hits are, in seconds.
	struct Loop
	{
		test::Audio audio;
		std::vector<double> hits;
		Noise noise;

		Loop(int sampleRate, int channels, double seconds)
		{
			audio.sampleRate = sampleRate;
			audio.channels = channels;
			audio.samples.assign(static_cast<size_t>(std::llround(seconds * sampleRate)) * channels, 0.0F);
		}

		// A synthesized drum: a kick's falling sine, a snare's noise and tone, a hi-hat's high noise; attack seconds
		// to full level. Hi-hats are louder on the right.
		void hit(Drum drum, double at, double velocity, double attack = 0.0)
		{
			const double rate = audio.sampleRate;
			double length = 0.4;
			if (drum == Drum::Hat)
				length = 0.08;
			else if (drum == Drum::Snare)
				length = 0.3;
			const auto first = std::llround(at * rate);
			double phase = 0.0;
			double before = 0.0;
			for (int64_t i = 0; i < std::llround(length * rate); ++i)
			{
				const double t = static_cast<double>(i) / rate;
				double value = 0.0;
				if (drum == Drum::Kick)
				{
					phase += 2.0 * g_pi * (50.0 + (100.0 * std::exp(-t / 0.03))) / rate;
					value = std::sin(phase) * std::exp(-t / 0.15);
				}
				else if (drum == Drum::Snare)
				{
					value = (0.6 * noise.next() * std::exp(-t / 0.05)) +
						(0.5 * std::sin(2.0 * g_pi * 190.0 * t) * std::exp(-t / 0.04));
				}
				else
				{
					const double white = noise.next();
					value = (white - before) * 0.5 * std::exp(-t / (drum == Drum::Hat ? 0.015 : 0.12));
					before = white;
				}
				value *= velocity * (attack > 0.0 ? std::min(1.0, t / attack) : 1.0);
				const auto frame = static_cast<size_t>(first + i);
				if (frame >= audio.frames())
					break;
				for (int c = 0; c < audio.channels; ++c)
				{
					const bool hat = drum == Drum::Hat || drum == Drum::OpenHat;
					const double pan = hat && audio.channels == 2 && c == 0 ? 0.6 : 1.0;
					audio.samples[(frame * audio.channels) + c] += static_cast<float>(value * pan);
				}
			}
			if (std::find(hits.begin(), hits.end(), at) == hits.end())
				hits.push_back(at);
			std::sort(hits.begin(), hits.end());
		}
	};

	// Two bars at 120 bpm, 48 kHz mono: kicks, snares with a ghost note, eighth hi-hats and an open one.
	Loop rockLoop()
	{
		Loop loop(48000, 1, 4.0);
		const double step = 0.125; // a sixteenth
		for (int bar = 0; bar < 2; ++bar)
		{
			const double at = bar * 16 * step;
			for (const auto& [sixteenth, velocity] :
				std::vector<std::pair<int, double>>{{0, 1.0}, {6, 0.7}, {8, 0.9}, {11, 0.6}})
				loop.hit(Drum::Kick, at + (sixteenth * step), velocity);
			loop.hit(Drum::Snare, at + (4 * step), 0.8);
			loop.hit(Drum::Snare, at + (12 * step), 0.85);
			loop.hit(Drum::Snare, at + (15 * step), 0.15); // a ghost note
			for (int eighth = 0; eighth < 8; ++eighth)
				if (bar == 0 || eighth != 7)
					loop.hit(Drum::Hat, at + (eighth * 2 * step), eighth % 2 == 0 ? 0.35 : 0.25);
		}
		loop.hit(Drum::OpenHat, 30 * step, 0.3);
		return loop;
	}

	// Two bars of swung sixteenths at 96 bpm, 44.1 kHz stereo: every hit has a 1 ms attack, under a noise floor at
	// -70 dBFS.
	Loop swingLoop()
	{
		Loop loop(44100, 2, 5.2);
		const double beat = 60.0 / 96.0;
		for (int bar = 0; bar < 2; ++bar)
			for (int sixteenth = 0; sixteenth < 16; ++sixteenth)
			{
				// Every second sixteenth comes a third of a sixteenth late.
				const int quarter = (bar * 4) + (sixteenth / 4);
				const double at =
					0.1 + (quarter * beat) + ((sixteenth % 4) * beat / 4.0) + (sixteenth % 2 == 1 ? beat / 12.0 : 0.0);
				if (sixteenth == 0 || sixteenth == 3 || sixteenth == 8 || sixteenth == 10)
					loop.hit(Drum::Kick, at, 0.9, 0.001);
				if (sixteenth == 4 || sixteenth == 12)
					loop.hit(Drum::Snare, at, 0.7, 0.001);
				loop.hit(Drum::Hat, at, sixteenth % 4 == 0 ? 0.4 : 0.25, 0.001);
			}
		Noise floor{777};
		for (float& sample : loop.audio.samples)
			sample += static_cast<float>(0.0003 * floor.next());
		return loop;
	}

	std::vector<engine::analysis::Onset> detect(const test::Audio& audio)
	{
		engine::analysis::OnsetDetector detector(audio.sampleRate, audio.channels);
		const size_t block = 1000; // blocks don't line up with the hits
		for (size_t frame = 0; frame < audio.frames(); frame += block)
			detector.feed(audio.samples.data() + (frame * audio.channels), std::min(block, audio.frames() - frame));
		return detector.finish();
	}

	// Every hit is found within 5 ms, and nothing else.
	void checkAccuracy(const Loop& loop)
	{
		const std::vector<engine::analysis::Onset> onsets = detect(loop.audio);
		const double tolerance = 0.005 * loop.audio.sampleRate;
		std::string found;
		for (const engine::analysis::Onset& onset : onsets)
			found += std::to_string(static_cast<double>(onset.frame) / loop.audio.sampleRate) + " ";
		INFO("found " << found);
		size_t matched = 0;
		double error = 0.0;
		for (const double hit : loop.hits)
		{
			const double frame = hit * loop.audio.sampleRate;
			const auto near = std::find_if(onsets.begin(), onsets.end(), [&](const engine::analysis::Onset& onset)
				{ return std::abs(static_cast<double>(onset.frame) - frame) <= tolerance; });
			INFO("the hit at " << hit << " s");
			CHECK(near != onsets.end());
			if (near != onsets.end())
			{
				++matched;
				error = std::max(error, std::abs(static_cast<double>(near->frame) - frame));
			}
		}
		CHECK(matched == loop.hits.size());
		CHECK(onsets.size() == loop.hits.size());
		CHECK(error <= 0.002 * loop.audio.sampleRate); // and mostly far closer
	}

	struct Fixture
	{
		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			const engine::Result result = client.call(command, parameters);
			INFO(command << ": " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		std::string fails(const std::string& command, const Json& parameters)
		{
			const engine::Result result = client.call(command, parameters);
			REQUIRE(!result.ok());
			return result.error;
		}

		// The rock loop on a linear audio track at bar 2 (two seconds at 120 bpm), the whole clip.
		Fixture()
		{
			const Loop loop = rockLoop();
			hits = loop.hits;
			file = (dir.path() / "loop.wav").u8string();
			test::writeWav(file, loop.audio);
			track = call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id").get<int64_t>();
			const Json media = call("media.add",
				{{"path", file}, {"sampleRate", 48000}, {"channels", 1},
					{"length", static_cast<int64_t>(loop.audio.frames())}})
								   .at("media");
			clip = call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
			event = call("audioEvent.add",
				{{"track", track}, {"clip", clip}, {"position", 2 * 48000}, {"offset", 0},
					{"length", static_cast<int64_t>(loop.audio.frames())}})
						.at("event")
						.at("id")
						.get<int64_t>();
		}

		// Waits until a job is done and returns it as job.get has it.
		Json waitForJob(const Json& started)
		{
			const auto job = started.at("job").at("id").get<uint64_t>();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			while (std::chrono::steady_clock::now() < deadline)
			{
				client.wait(std::chrono::milliseconds(20));
				if (Json state = call("job.get", {{"job", job}}).at("job"); state.at("state") != "running")
					return state;
			}
			FAIL_CHECK("the job didn't finish");
			return Json::object();
		}

		// Detects the clip's hitpoints and returns the job's result: clip.setHitpoints' with them.
		Json detect(int64_t id = 0)
		{
			const Json finished = waitForJob(call("clip.detectHitpoints", {{"clip", id != 0 ? id : clip}}));
			INFO(finished.dump());
			REQUIRE(finished.at("state") == "done");
			CHECK(finished.at("progress") == 1.0);
			return finished.at("result");
		}

		Json clipJson()
		{
			const Json clips = call("project.get").at("clips");
			const auto found =
				std::find_if(clips.begin(), clips.end(), [this](const Json& item) { return item.at("id") == clip; });
			REQUIRE(found != clips.end());
			return *found;
		}

		std::vector<int64_t> active()
		{
			std::vector<int64_t> positions;
			const Json item = clipJson();
			for (const Json& hitpoint : item.at("hitpoints"))
				if (hitpoint.at("active").get<bool>())
					positions.push_back(hitpoint.at("position").get<int64_t>());
			return positions;
		}

		test::TempDir dir;
		std::string file;
		std::vector<double> hits;
		engine::Engine engine;
		engine::Client client{engine};
		int64_t track = 0;
		int64_t clip = 0;
		int64_t event = 0;
	};
}

TEST_CASE("transient detection finds the hits of drum loops", "[unit]")
{
	SECTION("straight eighths with a ghost note and an open hi-hat")
	{
		checkAccuracy(rockLoop());
	}
	SECTION("swung sixteenths with soft attacks over noise, in stereo")
	{
		checkAccuracy(swingLoop());
	}
	SECTION("silence and steady tones have none")
	{
		test::Audio audio;
		audio.samples.assign(48000, 0.0F);
		CHECK(detect(audio).empty());
		for (int frame = 0; frame < 48000; ++frame)
			audio.samples[frame] = static_cast<float>(0.0005 * std::sin(frame * 2.0 * g_pi * 1000.0 / 48000.0));
		CHECK(detect(audio).empty()); // below -60 dBFS
		for (int frame = 0; frame < 48000; ++frame)
			audio.samples[frame] = static_cast<float>(0.5 * std::sin(frame * 2.0 * g_pi * 60.0 / 48000.0));
		CHECK(detect(audio).size() == 1); // where it starts
	}
}

TEST_CASE("clips have hitpoints, detected, filtered and edited as undo steps and saved with the project", "[unit]")
{
	Fixture f;
	CHECK(f.clipJson().at("hitpoints").empty());
	Json result = f.detect();
	CHECK(result.at("clip") == f.clip);
	const Json detected = result.at("hitpoints");
	REQUIRE(detected.size() == f.hits.size());
	CHECK(f.call("history.get").at("steps").back().at("action") == "Set hitpoints");
	double sharpest = 0.0;
	for (const Json& hitpoint : detected)
	{
		CHECK(hitpoint.at("active") == true); // the filters let every one through at first
		CHECK(hitpoint.at("locked") == false);
		sharpest = std::max(sharpest, hitpoint.at("intensity").get<double>());
	}
	CHECK(sharpest == 1.0);
	CHECK_THAT(f.fails("clip.detectHitpoints", {{"clip", 999}}), ContainsSubstring("no clip 999"));

	// The ghost note's peak is far below the others: the threshold leaves it out, and only it.
	const int64_t ghost = std::llround(f.hits.at(g_ghost) * 48000);
	REQUIRE(std::abs(detected.at(g_ghost).at("position").get<int64_t>() - ghost) < 240);
	const double ghostPeak = detected.at(g_ghost).at("peak").get<double>();
	result = f.call("clip.setHitpointFilter", {{"clip", f.clip}, {"threshold", ghostPeak + 1.0}});
	CHECK(f.active().size() == f.hits.size() - 1);
	CHECK(result.at("hitpoints").at(g_ghost).at("active") == false);
	CHECK(f.clipJson().at("hitpointFilter").at("threshold") == ghostPeak + 1.0);
	CHECK_THAT(
		f.fails("clip.setHitpointFilter", {{"clip", f.clip}, {"threshold", 1.0}}), ContainsSubstring("-60 to 0"));
	CHECK_THAT(f.fails("clip.setHitpointFilter", {{"clip", f.clip}, {"intensity", 2.0}}), ContainsSubstring("0 to 1"));

	// Locked hitpoints count whatever the filters; moving one locks it.
	const int64_t ghostAt = detected.at(g_ghost).at("position").get<int64_t>();
	f.call("clip.setHitpoint", {{"clip", f.clip}, {"position", ghostAt}, {"locked", true}});
	CHECK(f.active().size() == f.hits.size());
	// A minimum length of a quarter note leaves the sixteenths and eighths after a counting one out.
	f.call("clip.setHitpointFilter", {{"clip", f.clip}, {"threshold", -60.0}, {"minimumLength", 24000}});
	std::vector<int64_t> active = f.active();
	for (size_t i = 1; i < active.size(); ++i)
		CHECK((active[i] - active[i - 1] >= 24000 || active[i] == ghostAt || active[i - 1] == ghostAt));
	f.call("clip.setHitpointFilter", {{"clip", f.clip}, {"minimumLength", 0}});

	// Disabled ones never count; both false reset a hitpoint.
	const int64_t first = detected.at(0).at("position").get<int64_t>();
	f.call("clip.setHitpoint", {{"clip", f.clip}, {"position", first}, {"disabled", true}});
	CHECK(f.active().front() != first);
	f.call("clip.setHitpoint", {{"clip", f.clip}, {"position", ghostAt}, {"locked", false}, {"disabled", false}});
	f.call("clip.setHitpoint", {{"clip", f.clip}, {"position", first}, {"locked", false}, {"disabled", false}});
	CHECK(f.active().size() == f.hits.size());

	// Inserting and moving.
	result = f.call("clip.addHitpoint", {{"clip", f.clip}, {"position", 1000}});
	CHECK(result.at("hitpoints").at(1).at("position") == 1000); // by position
	CHECK(result.at("hitpoints").at(1).at("locked") == true);
	CHECK(f.call("history.get").at("steps").back().at("action") == "Insert hitpoint");
	CHECK_THAT(f.fails("clip.addHitpoint", {{"clip", f.clip}, {"position", 1000}}), ContainsSubstring("already"));
	CHECK_THAT(
		f.fails("clip.addHitpoint", {{"clip", f.clip}, {"position", 192000}}), ContainsSubstring("within the clip"));
	result = f.call("clip.setHitpoint", {{"clip", f.clip}, {"position", first}, {"moveTo", 2000}});
	CHECK(result.at("hitpoints").at(0).at("position") == 1000);
	CHECK(result.at("hitpoints").at(1).at("position") == 2000);
	CHECK(result.at("hitpoints").at(1).at("locked") == true);
	CHECK_THAT(f.fails("clip.setHitpoint", {{"clip", f.clip}, {"position", 5}}), ContainsSubstring("no hitpoint at 5"));
	CHECK_THAT(f.fails("clip.setHitpoint", {{"clip", f.clip}, {"position", 2000}, {"moveTo", 1000}}),
		ContainsSubstring("already"));
	f.call("history.undo");
	CHECK(f.clipJson().at("hitpoints").at(0).at("position") == first);

	// Saved in GNUbase's entry of the project file.
	const Json before = f.clipJson();
	const std::string file = (f.dir.path() / "hitpoints.dawproject").u8string();
	f.call("project.save", {{"path", file}, {"embedMedia", false}});
	f.call("project.new");
	f.call("project.load", {{"path", file}});
	const Json loaded = f.call("project.get").at("clips").at(0);
	CHECK(loaded.at("hitpoints") == before.at("hitpoints"));
	CHECK(loaded.at("hitpointFilter") == before.at("hitpointFilter"));

	// Remove All, and detecting them again.
	f.clip = loaded.at("id").get<int64_t>();
	CHECK(f.call("clip.removeHitpoints", {{"clip", f.clip}}).at("hitpoints").empty());
	CHECK(f.detect().at("hitpoints") == detected);
}

TEST_CASE("slices, events, regions, markers, a groove and MIDI notes are made of hitpoints", "[unit]")
{
	Fixture f;
	CHECK_THAT(f.fails("event.fromHitpoints", {{"event", f.event}, {"create", "events"}}),
		ContainsSubstring("detect them first"));
	const Json detected = f.detect().at("hitpoints");
	const size_t count = detected.size();
	std::vector<int64_t> frames;
	for (const Json& hitpoint : detected)
		frames.push_back(hitpoint.at("position").get<int64_t>());
	const int64_t start = int64_t{2} * 48000;
	const auto events = [&f]() { return f.call("project.get").at("audioEvents"); };

	SECTION("events: the event cut at its hitpoints")
	{
		const Json made = f.call("event.fromHitpoints", {{"event", f.event}, {"create", "events"}}).at("events");
		CHECK(made.size() == count); // the first hitpoint is at the event's start
		CHECK(made.at(0) == f.event);
		const Json all = events();
		REQUIRE(all.size() == count);
		for (const Json& event : all)
		{
			const auto offset = event.at("offset").get<int64_t>();
			CHECK(std::find(frames.begin(), frames.end(), offset) != frames.end());
			CHECK(event.at("position") == start + offset);
		}
		CHECK(f.call("history.get").at("steps").back().at("action") == "Create from hitpoints");
		f.call("history.undo");
		CHECK(events().size() == 1);
	}
	SECTION("slices: every event of the clip in a part of its slices")
	{
		const Json copy = f.call("event.copy", {{"events", {f.event}}, {"delta", 10 * 48000}}).at("events");
		const Json parts = f.call("event.fromHitpoints", {{"event", f.event}, {"create", "slices"}}).at("events");
		REQUIRE(parts.size() == 2);
		const Json project = f.call("project.get");
		CHECK(project.at("audioParts").size() == 2);
		CHECK(project.at("audioEvents").size() == 2 * count);
		for (const Json& event : project.at("audioEvents"))
			CHECK(event.at("part") != 0);
		f.call("history.undo");
		CHECK(f.call("project.get").at("audioParts").empty());
	}
	SECTION("regions: from the event's start to the first, from each to the next and the last to the end")
	{
		f.call("event.resize", {{"events", {f.event}}, {"edge", "start"}, {"delta", 100}});
		const Json made = f.call("event.fromHitpoints", {{"event", f.event}, {"create", "regions"}}).at("regions");
		CHECK(made.size() == count); // the first hitpoint is now before the event
		const Json regions = f.clipJson().at("regions");
		REQUIRE(regions.size() == count);
		CHECK(regions.at(0).at("start") == 100);
		CHECK(regions.at(0).at("end") == frames.at(1));
		CHECK(regions.back().at("end") == 192000);
	}
	SECTION("markers: one at each, on a marker track added for them")
	{
		const Json made = f.call("event.fromHitpoints", {{"event", f.event}, {"create", "markers"}}).at("markers");
		CHECK(made.size() == count);
		const Json markers = f.call("marker.list").at("markers");
		REQUIRE(markers.size() == count);
		for (size_t i = 0; i < count; ++i)
			CHECK(std::abs(markers.at(i).at("positionSamples").get<int64_t>() - (start + frames.at(i))) <= 1);
	}
	SECTION("groove: the project's quantize settings")
	{
		const Json settings =
			f.call("event.fromHitpoints", {{"event", f.event}, {"create", "groove"}}).at("quantize").at("settings");
		const Json& groove = settings.at("groove");
		const int64_t bar = int64_t{4} * 1720320;
		CHECK(groove.at("length") == 2 * bar); // the event's two bars
		CHECK(settings.at("origin") == bar);   // starting at bar 2
		REQUIRE(groove.at("points").size() == count);
		for (size_t i = 0; i < count; ++i)
		{
			// Within 5 ms (a 25th of a quarter at 120 bpm) of the hit.
			const double expected = f.hits.at(i) * 2.0 * 1720320;
			CHECK(std::abs(groove.at("points").at(i).at("position").get<double>() - expected) < 1720320 / 25.0);
		}
		CHECK(f.call("quantize.get").at("settings") == settings);
	}
	SECTION("MIDI notes: a part with a note at each, on a new MIDI track")
	{
		const Json made = f.call(
			"event.fromHitpoints", {{"event", f.event}, {"create", "midiNotes"}, {"pitch", 38}, {"length", 1000}});
		const Json project = f.call("project.get");
		Json part;
		for (const Json& each : project.at("midiParts"))
			if (each.at("id") == made.at("part"))
				part = each;
		REQUIRE(!part.is_null());
		CHECK(part.at("track") == made.at("track"));
		REQUIRE(part.at("notes").size() == count);
		double loudest = 0.0;
		for (const Json& note : part.at("notes"))
		{
			CHECK(note.at("pitch") == 38);
			CHECK(note.at("length") == 1000);
			loudest = std::max(loudest, note.at("velocity").get<double>());
		}
		CHECK(loudest == 1.0);
		CHECK(part.at("notes").at(g_ghost).at("velocity").get<double>() < 0.3); // the ghost note

		const Json fixed = f.call("event.fromHitpoints",
			{{"event", f.event}, {"create", "midiNotes"}, {"velocityMode", "fixed"}, {"velocity", 0.5},
				{"track", made.at("track")}});
		CHECK(fixed.at("track") == made.at("track"));
		CHECK_THAT(f.fails("event.fromHitpoints", {{"event", f.event}, {"create", "midiNotes"}, {"track", f.track}}),
			ContainsSubstring("isn't a MIDI or instrument track"));
	}
}

TEST_CASE("a sound is measured where it starts as the detection measures the onsets it finds", "[unit]")
{
	for (const Loop& loop : {rockLoop(), swingLoop()})
	{
		const test::Audio& audio = loop.audio;
		const auto width = static_cast<size_t>(audio.channels);
		const int64_t lead = engine::analysis::OnsetDetector::lead(audio.sampleRate);
		for (const engine::analysis::Onset& onset : detect(audio))
		{
			const auto from = static_cast<size_t>(std::max<int64_t>(0, onset.frame - lead));
			const engine::analysis::Onset measured = engine::analysis::OnsetDetector::measure(audio.sampleRate,
				audio.channels, audio.samples.data() + (from * width), audio.frames() - from,
				static_cast<size_t>(onset.frame) - from);
			INFO("the onset at frame " << onset.frame);
			CHECK(measured.frame == onset.frame - static_cast<int64_t>(from));
			CHECK(std::abs(measured.rise - onset.rise) < 1e-9);
			CHECK(measured.peak >= onset.peak - 1e-9); // over the whole 20 ms, which the detection starts later
			CHECK(measured.peak < onset.peak + 0.5);
		}
	}
	// Nothing to measure: no rise, and silence far below -60 dBFS.
	const std::vector<float> silence(1000, 0.0F);
	const engine::analysis::Onset quiet = engine::analysis::OnsetDetector::measure(48000, 1, silence.data(), 1000, 500);
	CHECK(quiet.rise == 0.0);
	CHECK(quiet.peak < -100.0);
	CHECK(engine::analysis::OnsetDetector::measure(48000, 1, silence.data(), 1000, 1000).rise == 0.0);
}

TEST_CASE("hitpoints inserted or moved by hand get the peak and intensity of the sound where they are", "[unit]")
{
	Fixture f;
	const Json detected = f.detect().at("hitpoints");
	REQUIRE(detected.size() == f.hits.size());
	const Json& ghost = detected.at(g_ghost);
	const auto at = ghost.at("position").get<int64_t>();
	const auto ghostPeak = ghost.at("peak").get<double>();
	const auto ghostIntensity = ghost.at("intensity").get<double>();
	const auto find = [&f](int64_t position)
	{
		const Json hitpoints = f.clipJson().at("hitpoints");
		const auto found = std::find_if(hitpoints.begin(), hitpoints.end(),
			[position](const Json& hitpoint) { return hitpoint.at("position") == position; });
		REQUIRE(found != hitpoints.end());
		return *found;
	};

	// Moved into the decay 50 ms later, it is quieter and its level doesn't rise there.
	f.call("clip.setHitpoint", {{"clip", f.clip}, {"position", at}, {"moveTo", at + 2400}});
	const Json moved = find(at + 2400);
	CHECK(moved.at("peak").get<double>() < ghostPeak - 3.0);
	CHECK(moved.at("intensity") == 0.0);
	CHECK(moved.at("locked") == true);

	// Inserted where the ghost note starts, it has what the detection found there: so a threshold below its peak
	// keeps it once it is unlocked, and the MIDI note it makes is as soft as the detected one's.
	f.call("clip.addHitpoint", {{"clip", f.clip}, {"position", at}});
	const Json inserted = find(at);
	CHECK(std::abs(inserted.at("peak").get<double>() - ghostPeak) < 0.5);
	CHECK(std::abs(inserted.at("intensity").get<double>() - ghostIntensity) < 1e-6);
	f.call("clip.setHitpoint", {{"clip", f.clip}, {"position", at}, {"locked", false}});
	f.call("clip.setHitpoint", {{"clip", f.clip}, {"position", at + 2400}, {"disabled", true}});
	f.call("clip.setHitpointFilter", {{"clip", f.clip}, {"threshold", ghostPeak - 6.0}});
	CHECK(f.active().size() == f.hits.size());
	const Json made = f.call("event.fromHitpoints", {{"event", f.event}, {"create", "midiNotes"}});
	Json notes;
	const Json project = f.call("project.get");
	for (const Json& part : project.at("midiParts"))
		if (part.at("id") == made.at("part"))
			notes = part.at("notes");
	REQUIRE(notes.size() == f.hits.size());
	const auto velocity = notes.at(g_ghost).at("velocity").get<double>();
	CHECK(velocity > 2.0 / 127.0);
	CHECK(velocity < 0.3);

	// Moving the sharpest one measures it against the next sharpest: back where it was, it is the sharpest again.
	size_t sharpest = 0;
	for (size_t i = 0; i < detected.size(); ++i)
		if (detected.at(i).at("intensity") == 1.0)
			sharpest = i;
	const auto top = detected.at(sharpest).at("position").get<int64_t>();
	f.call("clip.setHitpoint", {{"clip", f.clip}, {"position", top}, {"moveTo", top + 1}});
	f.call("clip.setHitpoint", {{"clip", f.clip}, {"position", top + 1}, {"moveTo", top}});
	CHECK(find(top).at("intensity") == 1.0);
	CHECK(std::abs(find(top).at("peak").get<double>() - detected.at(sharpest).at("peak").get<double>()) < 0.5);
}

TEST_CASE("clip.setHitpoints sets a clip's hitpoints as given and checks them", "[unit]")
{
	Fixture f;
	const Json given = {{{"position", 3000}, {"peak", -12.0}, {"intensity", 0.5}, {"locked", true}},
		{{"position", 1000}, {"peak", -6.0}, {"intensity", 1.0}, {"disabled", true}, {"active", true}}};
	const Json result = f.call("clip.setHitpoints", {{"clip", f.clip}, {"hitpoints", given}});
	REQUIRE(result.at("hitpoints").size() == 2);
	// By position, active as the filters have it.
	CHECK(result.at("hitpoints").at(0) ==
		Json{{"position", 1000}, {"peak", -6.0}, {"intensity", 1.0}, {"locked", false}, {"disabled", true},
			{"active", false}});
	CHECK(result.at("hitpoints").at(1).at("locked") == true);
	CHECK(f.call("history.get").at("steps").back().at("action") == "Set hitpoints");
	// They can be given back as they come.
	CHECK(f.call("clip.setHitpoints", {{"clip", f.clip}, {"hitpoints", result.at("hitpoints")}}) == result);

	const auto set = [&f](const Json& hitpoint)
	{ return f.fails("clip.setHitpoints", {{"clip", f.clip}, {"hitpoints", Json::array({hitpoint})}}); };
	CHECK_THAT(set({{"position", -1}, {"peak", 0.0}, {"intensity", 0.0}}), ContainsSubstring("within the clip"));
	CHECK_THAT(set({{"position", 192000}, {"peak", 0.0}, {"intensity", 0.0}}), ContainsSubstring("within the clip"));
	CHECK_THAT(set({{"position", 0}, {"peak", 0.0}, {"intensity", 1.5}}), ContainsSubstring("0 to 1"));
	CHECK_THAT(set({{"position", 0}, {"peak", 0.0}}), ContainsSubstring("intensity"));
	const Json twice = {
		{{"position", 5}, {"peak", 0.0}, {"intensity", 0.0}}, {{"position", 5}, {"peak", -1.0}, {"intensity", 0.0}}};
	CHECK_THAT(f.fails("clip.setHitpoints", {{"clip", f.clip}, {"hitpoints", twice}}),
		ContainsSubstring("two hitpoints at 5"));
	CHECK_THAT(
		f.fails("clip.setHitpoints", {{"clip", 999}, {"hitpoints", Json::array()}}), ContainsSubstring("no clip 999"));
	CHECK(f.clipJson().at("hitpoints") == result.at("hitpoints")); // nothing changed
}

TEST_CASE("detecting hitpoints runs as a job, which can be cancelled and fails without the clip or its file", "[unit]")
{
	Fixture f;
	// A minute of the loop, so that the job still runs when it is cancelled or the clip removed.
	const Loop loop = rockLoop();
	test::Audio minute;
	minute.sampleRate = loop.audio.sampleRate;
	minute.channels = loop.audio.channels;
	for (int i = 0; i < 15; ++i)
		minute.samples.insert(minute.samples.end(), loop.audio.samples.begin(), loop.audio.samples.end());
	const std::string file = (f.dir.path() / "minute.wav").u8string();
	test::writeWav(file, minute);
	const Json media = f.call("media.add",
							{{"path", file}, {"sampleRate", 48000}, {"channels", 1},
								{"length", static_cast<int64_t>(minute.frames())}})
						   .at("media");
	const auto clip = f.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
	const size_t steps = f.call("history.get").at("steps").size();

	SECTION("cancelled, it leaves the hitpoints as they were")
	{
		const Json started = f.call("clip.detectHitpoints", {{"clip", clip}});
		CHECK(started.at("job").at("kind") == "clip.detectHitpoints");
		f.call("job.cancel", {{"job", started.at("job").at("id")}});
		CHECK(f.waitForJob(started).at("state") == "cancelled");
		CHECK(f.call("history.get").at("steps").size() == steps);
	}
	SECTION("a clip removed meanwhile fails the job")
	{
		const Json started = f.call("clip.detectHitpoints", {{"clip", clip}});
		f.call("clip.remove", {{"clip", clip}});
		const Json finished = f.waitForJob(started);
		CHECK(finished.at("state") == "failed");
		CHECK_THAT(finished.at("error").get<std::string>(), ContainsSubstring("no clip " + std::to_string(clip)));
	}
	SECTION("a file it can't read fails the job, and inserting or moving hitpoints")
	{
		const Json missing = f.call("media.add",
								  {{"path", (f.dir.path() / "missing.wav").u8string()}, {"sampleRate", 48000},
									  {"channels", 1}, {"length", 48000}})
								 .at("media");
		const auto gone = f.call("clip.add", {{"media", missing.at("id")}}).at("clip").at("id").get<int64_t>();
		const Json one = {{{"position", 100}, {"peak", -6.0}, {"intensity", 1.0}}};
		f.call("clip.setHitpoints", {{"clip", gone}, {"hitpoints", one}});
		const Json before = f.call("project.get").at("clips");
		const Json finished = f.waitForJob(f.call("clip.detectHitpoints", {{"clip", gone}}));
		CHECK(finished.at("state") == "failed");
		CHECK_THAT(finished.at("error").get<std::string>(), ContainsSubstring("can't read"));
		CHECK_THAT(f.fails("clip.addHitpoint", {{"clip", gone}, {"position", 50}}), ContainsSubstring("can't read"));
		CHECK_THAT(f.fails("clip.setHitpoint", {{"clip", gone}, {"position", 100}, {"moveTo", 50}}),
			ContainsSubstring("can't read"));
		CHECK(f.call("project.get").at("clips") == before);
	}
}
