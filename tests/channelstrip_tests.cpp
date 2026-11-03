// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/automation.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// The channel strip's sections (#107, docs/features/channel-strip.md), measured with audio.analyze as an MCP client
// would.
using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace
{
	constexpr int64_t g_block = 4800; // frames measured at a time, after twice as many that let changes settle

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	// A stereo audio track that monitors a sine on the device input, rendered offline.
	class Strip
	{
	public:
		explicit Strip(double frequency = 1000.0, double dbfs = -6.0)
		{
			channel = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
			run(client, "channel.set", {{"channel", channel}, {"monitor", true}});
			run(client, "audio.setDevice",
				{{"sampleRate", 48000.0}, {"blockSize", 64}, {"captureSeconds", 60},
					{"input",
						{{"processor", "sine"},
							{"parameters", {{"Frequency", frequency}, {"Level", std::pow(10.0, dbfs / 20.0)}}}}}});
		}

		Json set(Json parameters)
		{
			parameters["channel"] = channel;
			return run(client, "channel.set", parameters).at("channel");
		}

		// Renders three blocks and measures the left output in the last, once the sections have glided.
		Json measure()
		{
			run(client, "audio.render", {{"frames", 3 * g_block}});
			const auto captured = run(client, "audio.analyze", {{"channels", {0}}}).at("frames").get<int64_t>();
			return run(client, "audio.analyze", {{"channels", {0}}, {"start", captured - g_block}, {"frames", g_block}})
				.at("channels")
				.at(0);
		}

		double peakDb()
		{
			const double peak = measure().at("peak").get<double>();
			return peak > 0.0 ? 20.0 * std::log10(peak) : -200.0;
		}

		// Renders frames and returns the left output's peak in dB from frame `from` to `to` of them.
		double renderPeakDb(int64_t frames, int64_t from, int64_t to)
		{
			const auto start = run(client, "audio.analyze", {{"channels", {0}}}).at("frames").get<int64_t>() + from;
			run(client, "audio.render", {{"frames", frames}});
			const double peak =
				run(client, "audio.analyze", {{"channels", {0}}, {"start", start}, {"frames", to - from}})
					.at("channels")
					.at(0)
					.at("peak")
					.get<double>();
			return peak > 0.0 ? 20.0 * std::log10(peak) : -200.0;
		}

		// The last frames rendered, of the left output.
		std::vector<float> lastFrames(int64_t frames)
		{
			const auto captured = run(client, "audio.analyze", {{"channels", {0}}}).at("frames").get<int64_t>();
			return run(client, "audio.capture", {{"channel", 0}, {"start", captured - frames}, {"frames", frames}})
				.at("samples")
				.get<std::vector<float>>();
		}

		Engine engine;
		Client client{engine};
		ObjectId channel = 0;
	};
}

TEST_CASE("channel.set changes the strip's sections, checks them, undoes them and saves them", "[integration]")
{
	Strip strip;
	const Json initial = run(strip.client, "project.get").at("channels")[0];
	CHECK(initial.at("pre") ==
		Json({{"gain", 0.0}, {"phase", false}, {"lowCut", {{"on", false}, {"frequency", 20.0}, {"slope", 12}}},
			{"highCut", {{"on", false}, {"frequency", 20000.0}, {"slope", 12}}}}));
	CHECK(initial.at("eq").at("bands").size() == 4);
	CHECK(initial.at("eq").at("bands")[1] ==
		Json({{"band", 2}, {"on", false}, {"type", "bell"}, {"frequency", 500.0}, {"gain", 0.0}, {"q", 1.0}}));
	CHECK(initial.at("strip").at("compressor").at("on") == false);

	// Each object changes only what it has.
	const Json changed = strip.set({{"pre", {{"gain", 6.0}, {"lowCut", {{"on", true}, {"slope", 24}}}}},
		{"eq", {{"bands", {{{"band", 4}, {"type", "highCut"}, {"on", true}}}}}},
		{"strip", {{"limiter", {{"on", true}, {"threshold", -3.0}}}}}});
	CHECK(changed.at("pre").at("gain") == 6.0);
	CHECK(changed.at("pre").at("phase") == false);
	CHECK(changed.at("pre").at("lowCut") == Json({{"on", true}, {"frequency", 20.0}, {"slope", 24}}));
	CHECK(changed.at("eq").at("bands")[3].at("type") == "highCut");
	CHECK(changed.at("eq").at("bands")[3].at("frequency") == 8000.0);
	CHECK(changed.at("strip").at("limiter") == Json({{"on", true}, {"threshold", -3.0}, {"release", 50.0}}));
	const Json before = run(strip.client, "project.get");

	const auto refused = [&strip](Json parameters, const std::string& reason)
	{
		parameters["channel"] = strip.channel;
		const Result result = strip.client.call("channel.set", parameters);
		INFO(parameters.dump());
		CHECK_THAT(result.error, ContainsSubstring(reason));
	};
	refused({{"pre", {{"gain", 30.0}}}}, "pre.gain must be a number from -24 to 24");
	refused({{"pre", {{"highCut", {{"frequency", 30.0}}}}}}, "pre.highCut.frequency must be a number from 50");
	refused({{"pre", {{"lowCut", {{"slope", 18}}}}}}, "slope must be 6, 12, 24, 36 or 48");
	refused({{"eq", {{"bands", {{{"band", 2}, {"type", "lowShelf"}}}}}}}, "eq band 2 can't be of type");
	refused({{"eq", {{"bands", {{{"band", 5}}}}}}}, "band from 1 to 4");
	refused({{"strip", {{"compressor", {{"ratio", 0.5}}}}}}, "strip.compressor.ratio");
	refused({{"strip", {{"gate", {{"on", 1}}}}}}, "strip.gate.on must be true or false");
	const Json midi = run(strip.client, "track.add", {{"type", "midi"}}).at("channel");
	CHECK_THAT(strip.client.call("channel.set", {{"channel", midi.at("id")}, {"eq", Json::object()}}).error,
		ContainsSubstring("a MIDI channel has no pre, eq or strip section"));
	run(strip.client, "history.undo"); // the MIDI track
	CHECK(run(strip.client, "project.get") == before);

	run(strip.client, "history.undo");
	CHECK(run(strip.client, "project.get").at("channels")[0] == initial);
	run(strip.client, "history.redo");
	CHECK(run(strip.client, "project.get") == before);

	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "strip.dawproject").u8string();
	run(strip.client, "project.save", {{"path", file}});
	run(strip.client, "project.new");
	run(strip.client, "project.load", {{"path", file}});
	CHECK(run(strip.client, "project.get").at("channels")[0] == before.at("channels")[0]);
}

TEST_CASE("a strip with every section off passes its audio unchanged", "[integration]")
{
	Strip strip;
	Strip reference;
	strip.set({{"pre", {{"gain", 0.0}}}});
	run(strip.client, "audio.render", {{"frames", 3 * g_block}});
	run(reference.client, "audio.render", {{"frames", 3 * g_block}});
	CHECK(strip.lastFrames(g_block) == reference.lastFrames(g_block));
	// Sections that were on and went off again pass it unchanged too, once they have glided out.
	strip.set({{"eq", {{"bands", {{{"band", 2}, {"on", true}, {"gain", 6.0}}}}}}});
	run(strip.client, "audio.render", {{"frames", g_block}});
	strip.set({{"eq", {{"bands", {{{"band", 2}, {"on", false}}}}}}});
	run(strip.client, "audio.render", {{"frames", 3 * g_block}});
	run(reference.client, "audio.render", {{"frames", 4 * g_block}});
	CHECK(strip.lastFrames(g_block) == reference.lastFrames(g_block));
}

TEST_CASE("the pre section's gain, phase and cut filters change the output", "[integration]")
{
	Strip strip(1000.0, -6.0);
	CHECK_THAT(strip.peakDb(), WithinAbs(-6.0, 0.05));
	strip.set({{"pre", {{"gain", 6.0}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(0.0, 0.05));
	strip.set({{"pre", {{"gain", -12.0}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-18.0, 0.05));

	// The phase inverts the polarity: the output is the negated input.
	Strip reference(1000.0, -6.0);
	strip.set({{"pre", {{"gain", 0.0}, {"phase", true}}}});
	run(strip.client, "audio.render", {{"frames", 6 * g_block}});      // the gain glides to exactly -1
	run(reference.client, "audio.render", {{"frames", 15 * g_block}}); // as many as the strip got
	const std::vector<float> inverted = strip.lastFrames(g_block);
	const std::vector<float> original = reference.lastFrames(g_block);
	REQUIRE(inverted.size() == original.size());
	size_t wrong = 0;
	for (size_t i = 0; i < inverted.size(); ++i)
		wrong += inverted[i] == -original[i] ? 0 : 1;
	CHECK(wrong == 0);
	strip.set({{"pre", {{"phase", false}}}});

	// A 48 dB low cut at 8 kHz takes a 1 kHz sine down by about 144 dB, a 12 dB one by about 36 dB (38 with the
	// frequency warping of the digital filter this close to half the sample rate); a high cut at 125 Hz by about
	// 36 dB at 12 dB per octave.
	strip.set({{"pre", {{"lowCut", {{"on", true}, {"frequency", 8000.0}, {"slope", 48}}}}}});
	CHECK(strip.peakDb() < -100.0);
	strip.set({{"pre", {{"lowCut", {{"slope", 12}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-6.0 - 38.0, 1.0));
	strip.set({{"pre", {{"lowCut", {{"on", false}}}, {"highCut", {{"on", true}, {"frequency", 125.0}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-6.0 - 36.0, 1.0));
	strip.set({{"pre", {{"highCut", {{"on", false}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-6.0, 0.05));
}

TEST_CASE("the equalizer's bands boost and cut, and its bypass turns them off", "[integration]")
{
	Strip strip(1000.0, -12.0);
	// A bell at the sine's frequency changes it by its gain.
	strip.set({{"eq", {{"bands", {{{"band", 3}, {"on", true}, {"frequency", 1000.0}, {"gain", 9.0}}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-3.0, 0.1));
	strip.set({{"eq", {{"bands", {{{"band", 2}, {"on", true}, {"frequency", 1000.0}, {"gain", -15.0}}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-18.0, 0.1));
	strip.set({{"eq", {{"bypass", true}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-12.0, 0.05));
	strip.set({{"eq", {{"bypass", false}, {"bands", {{{"band", 2}, {"on", false}}, {{"band", 3}, {"on", false}}}}}}});

	// A low shelf far above the sine lifts it by its gain; band 4 as a high cut far below it takes it down.
	strip.set({{"eq", {{"bands", {{{"band", 1}, {"on", true}, {"frequency", 16000.0}, {"gain", 6.0}}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-6.0, 0.2));
	strip.set({{"eq",
		{{"bands",
			{{{"band", 1}, {"on", false}}, {{"band", 4}, {"on", true}, {"type", "highCut"}, {"frequency", 62.5}}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-12.0 - 48.0, 1.5)); // 4 octaves at 12 dB per octave
}

TEST_CASE("an equalizer band or cut filter turned on from all-off glides in", "[integration]")
{
	Strip strip(1000.0, -12.0);
	run(strip.client, "audio.render", {{"frames", g_block}});
	// A 1 kHz sine has its peak in every 48 frames: in the first millisecond after the change, 1/20 of the glide, the
	// level has barely moved.
	const auto glidesIn = [&strip](const Json& on, double target)
	{
		INFO(on.dump());
		strip.set(on);
		CHECK_THAT(strip.renderPeakDb(g_block, 0, 48), WithinAbs(-12.0, 1.0));
		CHECK_THAT(strip.peakDb(), WithinAbs(target, 1.0));
	};
	const Json bell = {{"eq", {{"bands", {{{"band", 3}, {"on", true}, {"frequency", 1000.0}, {"gain", 12.0}}}}}}};
	const Json bellOff = {{"eq", {{"bands", {{{"band", 3}, {"on", false}}}}}}};
	const Json cut = {{"pre", {{"lowCut", {{"on", true}, {"frequency", 2000.0}, {"slope", 12}}}}}};
	const Json cutOff = {{"pre", {{"lowCut", {{"on", false}}}}}};
	// Each equalizer the first time, the second once the cut has glided out but goes on running for its tail.
	glidesIn(cut, -12.0 - 12.0); // an octave at 12 dB per octave
	strip.set(cutOff);
	run(strip.client, "audio.render", {{"frames", g_block / 2}});
	glidesIn(bell, 0.0);
	strip.set(bellOff);
	// After an idle spell, longer than the filters' tail, too.
	run(strip.client, "audio.render", {{"frames", 3 * g_block}});
	glidesIn(cut, -12.0 - 12.0);
	strip.set(cutOff);
	run(strip.client, "audio.render", {{"frames", 3 * g_block}});
	glidesIn(bell, 0.0);
}

TEST_CASE("the equalizer's Q sets a bell's width, and band 1 can be a low cut", "[integration]")
{
	Strip strip(1000.0, -12.0);
	// A bell an octave above the sine lifts it more when it is wide than when it is narrow.
	strip.set({{"eq", {{"bands", {{{"band", 3}, {"on", true}, {"frequency", 2000.0}, {"gain", 12.0}, {"q", 0.5}}}}}}});
	const double wide = strip.peakDb();
	strip.set({{"eq", {{"bands", {{{"band", 3}, {"q", 8.0}}}}}}});
	const double narrow = strip.peakDb();
	CHECK(wide > -12.0 + 6.0);
	CHECK(narrow < -12.0 + 1.0);
	strip.set({{"eq", {{"bands", {{{"band", 3}, {"on", false}}}}}}});

	// Band 1 as a low cut far above the sine takes it down, at 12 dB per octave.
	strip.set({{"eq", {{"bands", {{{"band", 1}, {"on", true}, {"type", "lowCut"}, {"frequency", 8000.0}}}}}}});
	CHECK(strip.peakDb() < -12.0 - 30.0);
}

TEST_CASE("the strip's dynamics follow their attack and release times", "[integration]")
{
	// The pre gain, before the dynamics, steps the level, gliding with a time constant of 20 ms. Returns the peak 20
	// to 40 ms after a step up, which crosses the threshold at once, or 80 to 100 ms after a step down, which crosses
	// it later, with the time at `fast` and at `slow`.
	const auto compare =
		[](const char* section, const char* time, double fast, double slow, Json settings, double from, double to)
	{
		settings["on"] = true;
		const int64_t start = to > from ? 960 : 3840;
		const auto afterStep = [&](double value)
		{
			settings[time] = value;
			Strip strip(1000.0, -6.0);
			strip.set({{"pre", {{"gain", from}}}, {"strip", {{section, settings}}}});
			strip.peakDb();
			strip.set({{"pre", {{"gain", to}}}});
			return strip.renderPeakDb(g_block, start, start + 960);
		};
		return std::make_pair(afterStep(fast), afterStep(slow));
	};

	// The gate opens over its attack and closes over its release.
	const Json gate = {{"threshold", -20.0}, {"range", -60.0}};
	auto [quick, late] = compare("gate", "attack", 0.01, 100.0, gate, -24.0, 0.0);
	CHECK(quick > late + 6.0);
	std::tie(quick, late) = compare("gate", "release", 1.0, 4000.0, gate, 0.0, -24.0);
	CHECK(quick < late - 6.0);

	// The compressor takes the level down over its attack and lets it up over its release.
	const Json compressor = {{"threshold", -24.0}, {"ratio", 8.0}};
	std::tie(quick, late) = compare("compressor", "attack", 0.1, 100.0, compressor, -24.0, 0.0);
	CHECK(quick < late - 6.0);
	std::tie(quick, late) = compare("compressor", "release", 10.0, 2000.0, compressor, 0.0, -24.0);
	CHECK(quick > late + 6.0);

	// The limiter lets the level up over its release; it has no attack, it looks ahead.
	std::tie(quick, late) = compare("limiter", "release", 1.0, 1000.0, {{"threshold", -20.0}}, 12.0, -20.0);
	CHECK(quick > late + 6.0);
}

TEST_CASE("the strip's gate, compressor and limiter shape the dynamics", "[integration]")
{
	Strip strip(1000.0, -6.0);
	// The gate closes below its threshold, down to its range, and lets louder signals through.
	strip.set({{"strip", {{"gate", {{"on", true}, {"threshold", -3.0}, {"range", -40.0}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-46.0, 0.5));
	strip.set({{"strip", {{"gate", {{"threshold", -20.0}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-6.0, 0.05));
	strip.set({{"strip", {{"gate", {{"on", false}}}}}});

	// The compressor lowers what is above its threshold by its ratio, and its makeup raises the result.
	strip.set({{"strip", {{"compressor", {{"on", true}, {"threshold", -30.0}, {"ratio", 4.0}}}}}});
	const double compressed = strip.peakDb();
	CHECK(compressed < -20.0);
	CHECK(compressed > -26.0); // a sine's level wobbles through the attack and release
	strip.set({{"strip", {{"compressor", {{"makeup", 10.0}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(compressed + 10.0, 0.1));
	strip.set({{"strip", {{"compressor", {{"on", false}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-6.0, 0.05));

	// No sample leaves the limiter above its threshold; it comes after the pre gain.
	strip.set({{"pre", {{"gain", 12.0}}}, {"strip", {{"limiter", {{"on", true}, {"threshold", -10.0}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-10.0, 0.05));
	strip.set({{"strip", {{"limiter", {{"on", false}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(6.0, 0.05));
}

TEST_CASE("the pre section runs before the plugin slots, the equalizer and the dynamics after them", "[integration]")
{
	Strip strip(1000.0, -6.0);
	const Json slot = run(strip.client, "plugin.insert", {{"channel", strip.channel}, {"plugin", "builtin:limiter"}})
						  .at("slot")
						  .at("id");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (run(strip.client, "plugin.get", {{"slot", slot}}).at("slot").at("status") != "active" &&
		std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	run(strip.client, "audio.updatePlugins");
	constexpr uint32_t ceiling = 1; // the limiter plugin's ceiling in dB
	run(strip.client, "parameter.set",
		{{"channel", strip.channel}, {"target", {{"type", "plugin"}, {"slot", slot}, {"parameter", ceiling}}},
			{"value", -20.0}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-20.0, 0.1));

	// The pre gain drives the plugin, which still stops it at -20 dB; the EQ lifts what comes out of it.
	strip.set({{"pre", {{"gain", 12.0}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-20.0, 0.1));
	strip.set({{"eq", {{"bands", {{{"band", 3}, {"on", true}, {"frequency", 1000.0}, {"gain", 12.0}}}}}}});
	CHECK_THAT(strip.peakDb(), WithinAbs(-8.0, 0.2));
}

TEST_CASE("the strip's limiter adds its look-ahead to the channel's latency while it is on", "[unit]")
{
	const auto latency = [](StripPart part, bool limiter)
	{
		ChannelSections sections;
		sections.limiter.on = limiter;
		ChannelStrip strip(std::make_shared<ChannelState>(), nullptr, {}, std::nullopt, false, part, sections);
		strip.prepareFor(48000.0, 64);
		return strip.latency();
	};
	CHECK(latency(StripPart::Whole, false) == 0);
	CHECK(latency(StripPart::Whole, true) == 48); // 1 ms at 48 kHz
	CHECK(latency(StripPart::Fader, true) == 48);
	CHECK(latency(StripPart::Source, true) == 0); // the limiter comes after the plugin slots
}
