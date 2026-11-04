// SPDX-License-Identifier: GPL-3.0-or-later
// The metronome and count-in (#101): settings and their persistence, the click across tempo and signature changes
// (golden file), click patterns, count-ins, the MIDI click, click outputs and sample sounds.
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/metronome/clicks.h"
#include "engine/metronome/metronome.h"
#include "engine/midifile/smf.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
namespace test = gnubase::test;

namespace
{
	constexpr double g_rate = 16000.0;

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters)
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	ObjectId channelOf(Client& client, ObjectId track)
	{
		ObjectId channel = 0;
		const Json project = run(client, "project.get");
		for (const Json& t : project.at("tracks"))
			if (t.at("id").get<ObjectId>() == track)
				channel = t.at("channel").get<ObjectId>();
		REQUIRE(channel != 0);
		return channel;
	}

	std::vector<float> capture(Client& client, int channel, int64_t frames)
	{
		return run(client, "audio.capture", {{"channel", channel}, {"start", 0}, {"frames", frames}})
			.at("samples")
			.get<std::vector<float>>();
	}

	// A tempo track at 480 ticks per quarter: tempo changes (tick, BPM) and signature changes (tick, numerator,
	// denominator), imported into the project with midiFile.import.
	struct Signature
	{
		int64_t tick = 0;
		int numerator = 4;
		int denominator = 4;
	};

	void importTempo(Client& client, const test::TempDir& dir, const std::vector<std::pair<int64_t, double>>& tempo,
		const std::vector<Signature>& signatures)
	{
		namespace smf = gnubase::engine::midifile;
		smf::Track track;
		for (const auto& [tick, bpm] : tempo)
		{
			const auto micros = static_cast<uint32_t>(std::lround(60000000.0 / bpm));
			track.events.push_back(smf::metaEvent(tick, smf::g_metaTempo,
				{static_cast<uint8_t>(micros >> 16), static_cast<uint8_t>(micros >> 8), static_cast<uint8_t>(micros)}));
		}
		for (const Signature& signature : signatures)
		{
			uint8_t power = 0;
			while ((1 << power) < signature.denominator)
				++power;
			track.events.push_back(smf::metaEvent(
				signature.tick, smf::g_metaTimeSignature, {static_cast<uint8_t>(signature.numerator), power, 24, 8}));
		}
		std::stable_sort(track.events.begin(), track.events.end(),
			[](const smf::Event& a, const smf::Event& b) { return a.tick < b.tick; });
		track.end = track.events.back().tick;
		smf::File file;
		file.tracks.push_back(std::move(track));
		const std::filesystem::path path = dir.path() / "tempo.mid";
		smf::save(file, path.u8string());
		run(client, "midiFile.import", {{"path", path.u8string()}, {"tempo", true}, {"markers", false}});
	}

	// A project at 16 kHz on the dummy device.
	struct Session
	{
		explicit Session(int blockSize = 128, int outputChannels = 2)
		{
			run(client, "project.set", {{"sampleRate", g_rate}});
			run(client, "audio.setDevice",
				{{"sampleRate", g_rate}, {"blockSize", blockSize}, {"inputChannels", 0},
					{"outputChannels", outputChannels}, {"captureSeconds", 6}});
		}

		Engine engine;
		Client client{engine};
	};

	// A click's frame and its peak.
	struct Onset
	{
		int64_t frame = 0;
		float peak = 0.0f;
	};

	// Where sounds start after silence: a beep's first sample is 0, its second one isn't.
	std::vector<Onset> onsets(const std::vector<float>& audio)
	{
		std::vector<Onset> result;
		size_t quiet = 1000;
		for (size_t i = 0; i + 1 < audio.size(); ++i)
		{
			if (audio[i] != 0.0f)
			{
				if (quiet >= 100)
					result.push_back({static_cast<int64_t>(i) - 1, 0.0f});
				result.back().peak = std::max(result.back().peak, std::abs(audio[i]));
				quiet = 0;
			}
			else
			{
				++quiet;
			}
		}
		return result;
	}

	std::vector<int64_t> framesOf(const std::vector<Onset>& clicks)
	{
		std::vector<int64_t> frames;
		frames.reserve(clicks.size());
		for (const Onset& click : clicks)
			frames.push_back(click.frame);
		return frames;
	}

	// 4/4 at 240 BPM (a beat is 4000 frames at 16 kHz), 3/4 from bar 1, 4/4 again from bar 2 (frame 28000).
	void countInProject(Session& session, const test::TempDir& dir)
	{
		importTempo(session.client, dir, {{0, 240.0}}, {{0, 4, 4}, {1920, 3, 4}, {3360, 4, 4}});
	}
}

TEST_CASE("metronome.set changes the given settings, checks them and is no undo step", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json defaults = run(client, "metronome.get");
	CHECK(!defaults.contains("click")); // the switch is transport.set metronome
	CHECK(defaults.at("audio").at("outputs") == Json::array());
	CHECK(defaults.at("countIn").at("bars") == 1);
	CHECK(defaults.at("patterns") == Json::array());

	const auto output =
		run(client, "bus.add", {{"direction", "output"}, {"name", "Click"}}).at("bus").at("id").get<ObjectId>();
	const auto track = run(client, "track.add", {{"type", "instrument"}}).at("track").at("id").get<ObjectId>();
	const ObjectId instrument = channelOf(client, track);
	const size_t steps = run(client, "history.get").at("steps").size();
	const Json set = run(client, "metronome.set",
		{{"whilePlaying", false},
			{"audio",
				{{"outputs", {output}}, {"level", -3.0}, {"sounds", "samples"},
					{"beeps", {Json::object(), {{"frequency", 440.0}}}}}},
			{"midi", {{"enabled", true}, {"destination", instrument}, {"channel", 10}, {"notes", {{{"note", 37}}}}}},
			{"countIn",
				{{"enabled", true}, {"bars", 2}, {"signature", "custom"},
					{"customSignature", {{"numerator", 7}, {"denominator", 8}}}}},
			{"patterns", {{{"numerator", 4}, {"denominator", 4}, {"accents", {1, 0, 3, 4}}}}}});
	CHECK(set == run(client, "metronome.get"));
	CHECK(set.at("whilePlaying") == false);
	CHECK(set.at("whileRecording") == true);
	CHECK(set.at("audio").at("outputs") == Json::array({output}));
	CHECK(set.at("audio").at("level") == -3.0);
	CHECK(set.at("audio").at("sounds") == "samples");
	CHECK(set.at("audio").at("beeps")[0] == defaults.at("audio").at("beeps")[0]);
	CHECK(set.at("audio").at("beeps")[1].at("frequency") == 440.0);
	CHECK(set.at("audio").at("beeps")[1].at("level") == defaults.at("audio").at("beeps")[1].at("level"));
	CHECK(set.at("midi").at("destination") == instrument);
	CHECK(set.at("midi").at("channel") == 10);
	CHECK(set.at("midi").at("notes")[0].at("note") == 37);
	CHECK(set.at("midi").at("notes")[0].at("velocity") == 1.0);
	CHECK(set.at("countIn").at("signature") == "custom");
	CHECK(set.at("countIn").at("customSignature").at("numerator") == 7);
	CHECK(set.at("patterns").size() == 1);
	CHECK(run(client, "history.get").at("steps").size() == steps);

	// Patterns per signature, kept sorted; no accents go back to the default.
	run(client, "metronome.setPattern", {{"numerator", 6}, {"denominator", 8}, {"accents", {1, 0, 0, 2, 0, 0}}});
	run(client, "metronome.setPattern", {{"numerator", 3}, {"denominator", 4}, {"accents", {1, 2, 2}}});
	Json patterns = run(client, "metronome.get").at("patterns");
	REQUIRE(patterns.size() == 3);
	CHECK(patterns[0].at("numerator") == 3);
	CHECK(patterns[1].at("numerator") == 4);
	CHECK(patterns[2].at("accents") == Json::array({1, 0, 0, 2, 0, 0}));
	run(client, "metronome.setPattern", {{"numerator", 4}, {"denominator", 4}});
	patterns = run(client, "metronome.get").at("patterns");
	CHECK(patterns.size() == 2);

	// Invalid settings fail and change nothing.
	const Json before = run(client, "metronome.get");
	const auto input = run(client, "project.get").at("busses")[0].at("id").get<ObjectId>();
	const auto audioTrack = run(client, "track.add", {{"type", "audio"}}).at("track").at("id").get<ObjectId>();
	CHECK_THAT(reject(client, "metronome.set", {{"whilePlaying", true}, {"audio", {{"outputs", {input}}}}}),
		ContainsSubstring("no output bus"));
	CHECK_THAT(reject(client, "metronome.set", {{"midi", {{"destination", channelOf(client, audioTrack)}}}}),
		ContainsSubstring("not an instrument channel"));
	CHECK_THAT(reject(client, "metronome.set", {{"audio", {{"level", 7.0}}}}), ContainsSubstring("level"));
	CHECK_THAT(reject(client, "metronome.set", {{"audio", {{"beeps", {{{"frequency", 5.0}}}}}}}),
		ContainsSubstring("frequency"));
	CHECK_THAT(
		reject(client, "metronome.set",
			{{"audio",
				{{"beeps",
					Json::array({Json::object(), Json::object(), Json::object(), Json::object(), Json::object()})}}}}),
		ContainsSubstring("up to 4"));
	CHECK_THAT(reject(client, "metronome.set", {{"midi", {{"channel", 17}}}}), ContainsSubstring("channel"));
	CHECK_THAT(
		reject(client, "metronome.set", {{"midi", {{"notes", {{{"velocity", 1.5}}}}}}}), ContainsSubstring("velocity"));
	CHECK_THAT(reject(client, "metronome.set", {{"countIn", {{"bars", 0}}}}), ContainsSubstring("bars"));
	CHECK_THAT(reject(client, "metronome.set", {{"countIn", {{"bars", 17}}}}), ContainsSubstring("bars"));
	CHECK_THAT(
		reject(client, "metronome.set", {{"countIn", {{"customSignature", {{"numerator", 4}, {"denominator", 3}}}}}}),
		ContainsSubstring("denominator"));
	CHECK_THAT(
		reject(client, "metronome.set", {{"patterns", {{{"numerator", 4}, {"denominator", 4}, {"accents", {1, 5}}}}}}),
		ContainsSubstring("accents"));
	CHECK_THAT(reject(client, "metronome.set", {{"audio", {{"samples", {{{"path", "no such file.wav"}}}}}}}),
		ContainsSubstring("accent 1"));
	CHECK_THAT(reject(client, "metronome.setPattern",
				   {{"numerator", 4}, {"denominator", 4}, {"accents", std::vector<int>(65, 1)}}),
		ContainsSubstring("64"));
	CHECK_THAT(reject(client, "metronome.setPattern", {{"numerator", 0}, {"denominator", 4}, {"accents", {1}}}),
		ContainsSubstring("numerator"));
	CHECK_THAT(
		reject(client, "metronome.setPattern", {{"numerator", 4}, {"denominator", 4294967300LL}, {"accents", {1}}}),
		ContainsSubstring("denominator"));
	CHECK_THAT(
		reject(client, "metronome.setPattern", {{"numerator", 4}, {"denominator", 4}, {"accents", {1, 4294967297LL}}}),
		ContainsSubstring("accents"));
	CHECK(run(client, "metronome.get") == before);
}

TEST_CASE("the metronome settings are saved with the project, and files without them get the defaults", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	run(client, "metronome.set",
		{{"whileRecording", false}, {"audio", {{"level", -6.0}, {"beeps", {{{"frequency", 2000.0}}}}}},
			{"countIn", {{"enabled", true}, {"bars", 3}, {"signature", "project"}}}});
	run(client, "metronome.setPattern", {{"numerator", 7}, {"denominator", 8}, {"accents", {1, 3, 3, 2, 3, 2, 3}}});
	const Json saved = run(client, "metronome.get");
	const std::string path = (dir.path() / "song.dawproject").u8string();
	run(client, "project.save", {{"path", path}});
	run(client, "project.new");
	CHECK(run(client, "metronome.get").at("whileRecording") == true);
	run(client, "project.load", {{"path", path}});
	CHECK(run(client, "metronome.get") == saved);

	// Files from before the metronome, and values in a file that aren't valid.
	Json project = toJson(newProject());
	project.erase("metronome");
	CHECK(toJson(projectFromJson(project).metronome) == toJson(metronome::MetronomeSettings{}));
	project["metronome"] = {{"click", true}, {"audio", {{"level", "loud"}, {"outputs", "all"}}},
		{"countIn", {{"bars", 99}, {"signature", "never"}}}, {"midi", {{"channel", 3}}},
		{"patterns",
			{{{"numerator", 3}, {"denominator", 4}, {"accents", {1, 9}}},
				{{"numerator", 5}, {"denominator", 4}, {"accents", {1, 2, 2, 2, 2}}}}}};
	const metronome::MetronomeSettings loaded = projectFromJson(project).metronome;
	CHECK(loaded.level == 0.0);
	CHECK(loaded.outputs.empty());
	CHECK(loaded.countInBars == 1);
	CHECK(loaded.countInSignature == metronome::CountInSignature::Start);
	CHECK(loaded.channel == 3);
	REQUIRE(loaded.patterns.size() == 1);
	CHECK(loaded.patterns[0].signature.numerator == 5);
}

TEST_CASE("the click follows tempo and signature changes and the click patterns: golden file", "[integration]")
{
	// 4/4 and 3/4 at 240 BPM, 6/8, then 5/4 and 7/8 at 300 BPM.
	const test::TempDir dir;
	Session session;
	Client& client = session.client;
	importTempo(
		client, dir, {{0, 240.0}, {4800, 300.0}}, {{0, 4, 4}, {1920, 3, 4}, {3360, 6, 8}, {4800, 5, 4}, {7200, 7, 8}});
	run(client, "transport.set", {{"metronome", true}});
	run(client, "metronome.set", {{"audio", {{"level", -3.0}}}});
	run(client, "metronome.setPattern", {{"numerator", 6}, {"denominator", 8}, {"accents", {1, 0, 0, 2, 0, 0}}});
	run(client, "metronome.setPattern", {{"numerator", 5}, {"denominator", 4}, {"accents", {1, 2, 2, 4, 2}}});
	run(client, "metronome.setPattern", {{"numerator", 7}, {"denominator", 8}, {"accents", {1, 3, 3, 2, 3, 2, 3}}});
	run(client, "transport.play");
	constexpr int64_t frames = 67200;
	run(client, "audio.render", {{"frames", frames}});

	const std::vector<float> left = capture(client, 0, frames);
	const std::vector<float> right = capture(client, 1, frames);
	CHECK(left == right);
	CHECK(framesOf(onsets(left)) ==
		std::vector<int64_t>{0, 4000, 8000, 12000, 16000, 20000, 24000, 28000, 34000, 40000, 43200, 46400, 49600, 52800,
			56000, 57600, 59200, 60800, 62400, 64000, 65600});

	test::Audio audio;
	audio.sampleRate = static_cast<int>(g_rate);
	audio.samples = left;
	const test::AudioDiff diff = test::compareWithGolden("metronome-signatures", audio, 1e-5f); // sin() by platform
	INFO(diff.report);
	CHECK(diff.equal);
}

TEST_CASE("the click is the same for every block size and number of threads", "[integration]")
{
	const auto render = [](int blockSize, int threads)
	{
		const test::TempDir dir;
		Session session(blockSize);
		Client& client = session.client;
		run(client, "audio.setProcessing", {{"multiProcessing", threads > 1}, {"threads", threads}});
		run(client, "track.add", {{"type", "audio"}});
		importTempo(client, dir, {{0, 240.0}, {2400, 251.0}}, {{0, 4, 4}, {1920, 5, 8}});
		run(client, "transport.set", {{"metronome", true}});
		run(client, "metronome.set", {{"countIn", {{"enabled", true}, {"signature", "project"}}}});
		run(client, "metronome.setPattern", {{"numerator", 4}, {"denominator", 4}, {"accents", {1, 3, 2, 3, 2, 3}}});
		run(client, "transport.set", {{"left", 9000}, {"right", 25000}, {"cycle", true}});
		run(client, "transport.locate", {{"position", 5000}});
		run(client, "transport.record");
		run(client, "audio.render", {{"frames", 60000}});
		return capture(client, 0, 60000);
	};
	const std::vector<float> reference = render(128, 1);
	CHECK(onsets(reference).size() > 20);
	const int blockSize = GENERATE(1, 17, 4096);
	CHECK(render(blockSize, 1) == reference);
	CHECK(render(blockSize, 4) == reference);
}

TEST_CASE("a count-in clicks its bars, then playback starts at the exact sample where it ends", "[integration]")
{
	const test::TempDir dir;
	const int blockSize = GENERATE(17, 256, 4096);
	Session session(blockSize);
	Client& client = session.client;
	countInProject(session, dir);
	run(client, "transport.locate", {{"position", 28000}}); // bar 2
	const auto countIn = [&client, blockSize](const Json& settings, int64_t frames, bool click = false)
	{
		run(client, "audio.setDevice",
			{{"sampleRate", g_rate}, {"blockSize", blockSize}, {"inputChannels", 0}, {"captureSeconds", 6}});
		run(client, "metronome.set", settings);
		run(client, "transport.set", {{"metronome", click}});
		run(client, "transport.record");
		run(client, "audio.render", {{"frames", frames}});
		const std::vector<Onset> clicks = onsets(capture(client, 0, frames));
		const auto position = run(client, "transport.get").at("position").get<int64_t>();
		run(client, "transport.stop");
		run(client, "transport.locate", {{"position", 28000}});
		return std::make_pair(clicks, position);
	};

	SECTION("in the signature and tempo at the start")
	{
		const auto [clicks, position] = countIn({{"countIn", {{"enabled", true}}}}, 20000);
		REQUIRE(framesOf(clicks) == std::vector<int64_t>{0, 4000, 8000, 12000});
		CHECK(clicks[0].peak > 1.5f * clicks[1].peak); // accent 1, then accent 2 at -6 dB
		CHECK(position == 28000 + 4000);
	}
	SECTION("the click plays on after the count-in if it is on")
	{
		const auto [clicks, position] = countIn({{"countIn", {{"enabled", true}, {"bars", 2}}}}, 36000, true);
		CHECK(framesOf(clicks) == std::vector<int64_t>{0, 4000, 8000, 12000, 16000, 20000, 24000, 28000, 32000});
		CHECK(position == 28000 + 4000);
	}
	SECTION("over the bars before the start as the project has them")
	{
		const auto [clicks, position] = countIn({{"countIn", {{"enabled", true}, {"signature", "project"}}}}, 13000);
		CHECK(framesOf(clicks) == std::vector<int64_t>{0, 4000, 8000}); // bar 1 in 3/4
		CHECK(position == 28000 + 1000);
	}
	SECTION("from the start of a bar before a start within a bar")
	{
		run(client, "transport.locate", {{"position", 30000}});
		const auto [clicks, position] =
			countIn({{"countIn", {{"enabled", true}, {"signature", "project"}}}}, 20000, true);
		CHECK(framesOf(clicks) == std::vector<int64_t>{0, 4000, 8000, 12000, 16000});
		CHECK(position == 30000 + 6000);
	}
	SECTION("in a custom signature")
	{
		const auto [clicks, position] = countIn({{"countIn",
													{{"enabled", true}, {"bars", 2}, {"signature", "custom"},
														{"customSignature", {{"numerator", 3}, {"denominator", 8}}}}}},
			13000);
		// Eighths at 240 BPM, three to a bar.
		REQUIRE(framesOf(clicks) == std::vector<int64_t>{0, 2000, 4000, 6000, 8000, 10000});
		CHECK(clicks[3].peak > 1.5f * clicks[4].peak);
		CHECK(position == 28000 + 1000);
	}
	SECTION("only if the count-in is on")
	{
		const auto [clicks, position] = countIn({{"countIn", {{"enabled", false}}}}, 4000);
		CHECK(clicks.empty());
		CHECK(position == 28000 + 4000);
	}
	SECTION("stop ends it, and the cursor stays")
	{
		run(client, "metronome.set", {{"countIn", {{"enabled", true}}}});
		run(client, "transport.record");
		run(client, "audio.render", {{"frames", 5000}});
		run(client, "transport.stop");
		run(client, "audio.render", {{"frames", 20000}});
		CHECK(framesOf(onsets(capture(client, 0, 25000))) == std::vector<int64_t>{0, 4000});
		const Json transport = run(client, "transport.get");
		CHECK(transport.at("playing") == false);
		CHECK(transport.at("position") == 28000);
	}
}

TEST_CASE("the MIDI click plays its notes on the destination instrument", "[integration]")
{
	const test::TempDir dir;
	Session session;
	Client& client = session.client;
	countInProject(session, dir);
	const auto track = run(client, "track.add", {{"type", "instrument"}}).at("track").at("id").get<ObjectId>();
	const ObjectId instrument = channelOf(client, track);
	run(client, "channel.set", {{"channel", instrument}, {"instrument", "noteProbe"}});
	run(client, "transport.set", {{"metronome", true}});
	run(client, "metronome.set",
		{{"audio", {{"enabled", false}}},
			{"midi", {{"enabled", true}, {"destination", instrument}, {"notes", {{{"note", 60}}, {{"note", 62}}}}}}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 20000}});

	// The probe: how many notes are on (channel 0) and the sum of their pitches (channel 1).
	const std::vector<float> count = capture(client, 0, 20000);
	const std::vector<float> pitches = capture(client, 1, 20000);
	for (const int64_t beat : {0, 4000, 8000, 12000, 16000})
	{
		INFO("beat at " << beat);
		const auto at = static_cast<size_t>(beat);
		CHECK(count[at] == 1.0f);
		CHECK(pitches[at] == (beat % 16000 == 0 ? 60.0f : 62.0f));
		CHECK(count[at + 799] == 1.0f); // 50 ms
		CHECK(count[at + 800] == 0.0f);
		CHECK(count[at + 3999] == 0.0f);
	}

	// A destination that is gone plays nothing.
	run(client, "transport.stop");
	run(client, "track.remove", {{"track", track}});
	run(client, "transport.locate", {{"position", 0}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 4000}});
	const std::vector<float> after = capture(client, 0, 24000);
	CHECK(std::all_of(after.begin() + 20000, after.end(), [](float s) { return s == 0.0f; }));
	run(client, "metronome.set", {{"midi", {{"channel", 2}}}}); // the settings can still change
}

TEST_CASE("the audio click plays through the chosen output busses", "[integration]")
{
	Session session(128, 4);
	Client& client = session.client;
	const auto click =
		run(client, "bus.add", {{"direction", "output"}, {"name", "Click"}}).at("bus").at("id").get<ObjectId>();
	run(client, "transport.set", {{"metronome", true}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 16000}});
	CHECK(framesOf(onsets(capture(client, 0, 16000))) == std::vector<int64_t>{0, 8000}); // 120 BPM
	CHECK(onsets(capture(client, 2, 16000)).empty());

	SECTION("another bus")
	{
		run(client, "metronome.set", {{"audio", {{"outputs", {click}}}}});
		run(client, "audio.render", {{"frames", 16000}});
		const std::vector<float> main = capture(client, 0, 32000);
		CHECK(std::all_of(main.begin() + 16000, main.end(), [](float s) { return s == 0.0f; }));
		CHECK(framesOf(onsets(capture(client, 2, 32000))) == std::vector<int64_t>{16000, 24000});
	}
	SECTION("a removed bus: the main mix again, and the settings can still change")
	{
		run(client, "metronome.set", {{"audio", {{"outputs", {click}}}}});
		run(client, "bus.remove", {{"bus", click}});
		run(client, "metronome.set", {{"audio", {{"level", -1.0}}}});
		run(client, "audio.render", {{"frames", 16000}});
		CHECK(framesOf(onsets(capture(client, 0, 32000))) == std::vector<int64_t>{0, 8000, 16000, 24000});
	}
	SECTION("without outputs, the main mix")
	{
		run(client, "bus.set", {{"bus", click}, {"mainMix", true}});
		run(client, "audio.render", {{"frames", 16000}});
		const std::vector<float> first = capture(client, 0, 32000);
		CHECK(std::all_of(first.begin() + 16000, first.end(), [](float s) { return s == 0.0f; }));
		CHECK(framesOf(onsets(capture(client, 2, 32000))) == std::vector<int64_t>{16000, 24000});
	}
	SECTION("audio off")
	{
		run(client, "metronome.set", {{"audio", {{"enabled", false}}}});
		run(client, "audio.render", {{"frames", 16000}});
		CHECK(onsets(capture(client, 0, 32000)).size() == 2);
	}
	SECTION("the metronome switch off")
	{
		run(client, "transport.set", {{"metronome", false}});
		run(client, "audio.render", {{"frames", 16000}});
		CHECK(onsets(capture(client, 0, 32000)).size() == 2);
		run(client, "transport.set", {{"metronome", true}});
		run(client, "audio.render", {{"frames", 16000}});
		CHECK(framesOf(onsets(capture(client, 0, 48000))) == std::vector<int64_t>{0, 8000, 32000, 40000});
	}
	SECTION("not while playing")
	{
		run(client, "metronome.set", {{"whilePlaying", false}});
		run(client, "audio.render", {{"frames", 16000}});
		CHECK(onsets(capture(client, 0, 32000)).size() == 2);
	}
}

TEST_CASE("sample files are click sounds; a missing one plays the beep", "[integration]")
{
	const test::TempDir dir;
	Session session;
	Client& client = session.client;
	test::Audio ramp;
	ramp.sampleRate = static_cast<int>(g_rate);
	for (int i = 1; i <= 100; ++i)
		ramp.samples.push_back(static_cast<float>(i) / 200.0f);
	const std::filesystem::path rampFile = dir.path() / "ramp.wav";
	test::writeWav(rampFile, ramp);
	test::Audio dc; // at twice the rate, in stereo: converted and mixed down
	dc.sampleRate = 32000;
	dc.channels = 2;
	dc.samples.assign(800, 0.25f);
	const std::filesystem::path dcFile = dir.path() / "dc.wav";
	test::writeWav(dcFile, dc);

	run(client, "transport.set", {{"metronome", true}});
	run(client, "metronome.set",
		{{"audio",
			{{"sounds", "samples"},
				{"samples", {{{"path", rampFile.u8string()}, {"level", -6.0}}, {{"path", dcFile.u8string()}}}}}}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 16000}});
	const std::vector<float> out = capture(client, 0, 16000);
	const double gain = 0.70794578438413791 * std::pow(10.0, -6.0 / 20.0); // mono to stereo, -6 dB
	for (size_t i = 0; i < 100; ++i)
		CHECK(std::abs(out[i] - static_cast<float>(ramp.samples[i] * gain)) < 1e-6f);
	CHECK(out[100] == 0.0f);
	CHECK(std::abs(out[8000 + 100] - (0.25f * 0.70794578f)) < 0.02f);
	CHECK(out[8000 + 300] == 0.0f);

	// A sample that is gone when the project loads plays the beep.
	const std::string project = (dir.path() / "song.dawproject").u8string();
	run(client, "project.save", {{"path", project}});
	run(client, "transport.stop");
	std::filesystem::remove(rampFile);
	run(client, "project.load", {{"path", project}});
	run(client, "transport.locate", {{"position", 0}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 8000}});
	const std::vector<float> again = capture(client, 0, 24000);
	const std::vector<Onset> clicks = onsets(std::vector<float>(again.begin() + 16000, again.end()));
	REQUIRE(clicks.size() == 1);
	CHECK(clicks[0].frame == 0);
	CHECK(std::abs(again[16000 + 400]) > 0.0f); // the beep lasts 50 ms, the ramp 100 frames
}

TEST_CASE("clicks are the same however a range is split", "[unit]")
{
	using namespace gnubase::engine::metronome;
	const gnubase::time::TempoMap tempo({{0, 120.0, false}, {gnubase::time::g_ticksPerQuarter * 6, 97.0, true}});
	const gnubase::time::SignatureMap signatures({{0, {4, 4}}, {1, {7, 8}}, {3, {5, 16}}});
	PatternTable patterns;
	PatternTable::Entry entry;
	entry.signature = {7, 8};
	entry.clicks = 5;
	entry.accents = {1, 0, 2, 3, 4};
	patterns.entries.push_back(entry);
	const ProjectTimeline timeline{tempo, signatures, 44100.0};
	std::vector<std::pair<int64_t, int>> whole;
	clicksIn(timeline, patterns, -50000, 600000, [&whole](int64_t at, int accent) { whole.emplace_back(at, accent); });
	CHECK(whole.size() > 30);
	for (const int64_t step : {1, 997, 22050})
	{
		std::vector<std::pair<int64_t, int>> split;
		for (int64_t at = -50000; at < 600000; at += step)
			clicksIn(timeline, patterns, at, std::min<int64_t>(at + step, 600000),
				[&split](int64_t position, int accent) { split.emplace_back(position, accent); });
		CHECK(split == whole);
	}

	// A constant timeline clicks like the project's at one tempo and signature.
	const gnubase::time::TempoMap steady(133.0);
	const gnubase::time::SignatureMap fiveFour({{0, {5, 4}}});
	std::vector<std::pair<int64_t, int>> project;
	std::vector<std::pair<int64_t, int>> constant;
	clicksIn(ProjectTimeline{steady, fiveFour, 48000.0}, patterns, 0, 480000,
		[&project](int64_t at, int accent) { project.emplace_back(at, accent); });
	clicksIn(ConstantTimeline{133.0, {5, 4}, 48000.0}, patterns, 0, 480000,
		[&constant](int64_t at, int accent) { constant.emplace_back(at, accent); });
	CHECK(constant == project);
}

TEST_CASE("the click sounds while playing or recording, as whilePlaying and whileRecording say", "[integration]")
{
	Session session;
	Client& client = session.client;
	run(client, "transport.set", {{"metronome", true}});
	int64_t rendered = 0;
	const auto clicks = [&client, &rendered](const char* command)
	{
		run(client, "transport.stop");
		run(client, "transport.locate", {{"position", 0}});
		run(client, command);
		run(client, "audio.render", {{"frames", 16000}});
		rendered += 16000;
		const std::vector<float> all = capture(client, 0, rendered);
		return onsets(std::vector<float>(all.end() - 16000, all.end())).size();
	};
	CHECK(clicks("transport.play") == 2);
	CHECK(clicks("transport.record") == 2);
	run(client, "metronome.set", {{"whileRecording", false}});
	CHECK(clicks("transport.play") == 2);
	CHECK(clicks("transport.record") == 0);
	run(client, "metronome.set", {{"whileRecording", true}, {"whilePlaying", false}});
	CHECK(clicks("transport.play") == 0);
	CHECK(clicks("transport.record") == 2);
}

TEST_CASE("only recording from stop counts in", "[integration]")
{
	Session session;
	Client& client = session.client;
	run(client, "metronome.set", {{"countIn", {{"enabled", true}}}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 16000}});
	CHECK(onsets(capture(client, 0, 16000)).empty());
	CHECK(run(client, "transport.get").at("position") == 16000);

	// Recording while playing punches in at once.
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 8000}});
	CHECK(onsets(capture(client, 0, 24000)).empty());
	const Json transport = run(client, "transport.get");
	CHECK(transport.at("recording") == true);
	CHECK(transport.at("position") == 24000);
}

TEST_CASE("a pickup bar clicks its beats, and the next bar starts with accent 1", "[integration]")
{
	Session session;
	Client& client = session.client;
	run(client, "signature.add", {{"bar", 0}, {"numerator", 1}, {"denominator", 4}});
	run(client, "signature.add", {{"bar", 1}, {"numerator", 4}, {"denominator", 4}});
	run(client, "transport.set", {{"metronome", true}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 48000}});
	const std::vector<Onset> clicks = onsets(capture(client, 0, 48000));
	REQUIRE(framesOf(clicks) == std::vector<int64_t>{0, 8000, 16000, 24000, 32000, 40000}); // 120 BPM
	CHECK(clicks[1].peak > 1.5f * clicks[2].peak);                                          // bar 1's first beat
	CHECK(clicks[5].peak > 1.5f * clicks[4].peak);                                          // bar 2's
}
