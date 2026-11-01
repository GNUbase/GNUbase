// SPDX-License-Identifier: GPL-3.0-or-later
// Plugins in slots in the audio engine (#282): a channel's audio passes its slots from top to bottom before the fader,
// a bypassed or inactive plugin passes it unchanged, and the engine makes, activates and destroys the plugins as the
// slots change.
#include "engine/document/document.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::WithinAbs;

namespace
{
	constexpr int64_t g_block = 4800; // frames measured at a time, after twice as many that let changes settle

	// The parametric EQ's third band, a bell at 500 Hz: its gain (dsp::Equalizer::parameter).
	constexpr uint32_t g_bellGain = 16;
	constexpr uint32_t g_gain = 0;           // gainPan's gain in dB
	constexpr uint32_t g_limiterCeiling = 1; // the limiter's ceiling in dB

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	// An audio track that monitors a sine of the peak level in dBFS on the device input, rendered offline.
	ObjectId monitorSine(Client& client, double frequency, double dbfs)
	{
		const ObjectId channel = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
		run(client, "channel.set", {{"channel", channel}, {"monitor", true}});
		run(client, "audio.setDevice",
			{{"sampleRate", 48000.0}, {"captureSeconds", 60},
				{"input",
					{{"processor", "sine"},
						{"parameters", {{"Frequency", frequency}, {"Level", std::pow(10.0, dbfs / 20.0)}}}}}});
		return channel;
	}

	// Renders three blocks and measures the left output's peak level in dBFS in the last: monitoring, the fader and
	// the EQ's parameters glide.
	double peakDb(Client& client)
	{
		run(client, "audio.render", {{"frames", 3 * g_block}});
		const auto captured = run(client, "audio.analyze", {{"channels", {0}}}).at("frames").get<int64_t>();
		const Json measured =
			run(client, "audio.analyze", {{"channels", {0}}, {"start", captured - g_block}, {"frames", g_block}});
		const double peak = measured.at("channels").at(0).at("peak").get<double>();
		return peak > 0.0 ? 20.0 * std::log10(peak) : -200.0;
	}

	std::string status(Client& client, const Json& slot)
	{
		return run(client, "plugin.get", {{"slot", slot}}).at("slot").at("status").get<std::string>();
	}

	// Waits until a slot's plugin has the status, then builds the graph with it, as the engine does on its own then.
	void waitFor(Client& client, const Json& slot, const std::string& wanted)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		std::string now = status(client, slot);
		while (now != wanted && std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			now = status(client, slot);
		}
		REQUIRE(now == wanted);
		run(client, "audio.updatePlugins");
	}

	Json insert(Client& client, ObjectId channel, const char* plugin)
	{
		return run(client, "plugin.insert", {{"channel", channel}, {"plugin", plugin}}).at("slot").at("id");
	}

	void setParameter(Client& client, ObjectId channel, const Json& slot, uint32_t parameter, double value)
	{
		run(client, "parameter.set",
			{{"channel", channel}, {"target", {{"type", "plugin"}, {"slot", slot}, {"parameter", parameter}}},
				{"value", value}});
	}
}

TEST_CASE(
	"a built-in effect in a slot processes the channel's audio before the fader, and undo removes it", "[integration]")
{
	Engine engine;
	Client client(engine);
	const ObjectId channel = monitorSine(client, 500.0, -24.0);
	CHECK_THAT(peakDb(client), WithinAbs(-24.0, 0.01));

	const Json slot = insert(client, channel, "builtin:eq");
	waitFor(client, slot, "active");
	CHECK_THAT(peakDb(client), WithinAbs(-24.0, 0.01)); // the EQ is flat by default
	setParameter(client, channel, slot, g_bellGain, 12.0);
	CHECK_THAT(peakDb(client), WithinAbs(-12.0, 0.1));
	const Json spectrum = run(client, "audio.analyze", {{"channels", {0}}, {"peaks", 1}});
	CHECK_THAT(spectrum.at("channels").at(0).at("spectrum").at("peaks").at(0).at("frequency").get<double>(),
		WithinAbs(500.0, 1.0));

	run(client, "channel.set", {{"channel", channel}, {"volume", -6.0}});
	CHECK_THAT(peakDb(client), WithinAbs(-18.0, 0.1));

	run(client, "history.undo"); // the volume
	run(client, "history.undo"); // the gain
	CHECK_THAT(peakDb(client), WithinAbs(-24.0, 0.01));
	setParameter(client, channel, slot, g_bellGain, 12.0);
	run(client, "history.undo");
	run(client, "history.undo"); // the plugin
	CHECK(run(client, "plugin.list", {{"channel", channel}}).at("slots").empty());
	CHECK_THAT(peakDb(client), WithinAbs(-24.0, 0.01));

	run(client, "history.redo"); // a new instance of the plugin
	waitFor(client, slot, "active");
	run(client, "history.redo");
	CHECK_THAT(peakDb(client), WithinAbs(-12.0, 0.1));
}

TEST_CASE("a bypassed or inactive plugin passes the audio unchanged", "[integration]")
{
	Engine engine;
	Client client(engine);
	const ObjectId channel = monitorSine(client, 500.0, -24.0);
	const Json slot = insert(client, channel, "builtin:eq");
	waitFor(client, slot, "active");
	setParameter(client, channel, slot, g_bellGain, 12.0);
	CHECK_THAT(peakDb(client), WithinAbs(-12.0, 0.1));

	run(client, "plugin.set", {{"slot", slot}, {"bypass", true}});
	CHECK(status(client, slot) == "active"); // a bypassed plugin keeps processing
	CHECK_THAT(peakDb(client), WithinAbs(-24.0, 0.01));
	run(client, "plugin.set", {{"slot", slot}, {"bypass", false}});
	CHECK_THAT(peakDb(client), WithinAbs(-12.0, 0.1));

	run(client, "plugin.set", {{"slot", slot}, {"active", false}});
	waitFor(client, slot, "inactive");
	CHECK_THAT(peakDb(client), WithinAbs(-24.0, 0.01));
	run(client, "plugin.set", {{"slot", slot}, {"active", true}});
	waitFor(client, slot, "active");
	CHECK_THAT(peakDb(client), WithinAbs(-12.0, 0.1)); // with its parameters
}

TEST_CASE("the audio passes the slots from top to bottom", "[integration]")
{
	Engine engine;
	Client client(engine);
	const ObjectId channel = monitorSine(client, 1000.0, -6.0);
	const Json gain = insert(client, channel, "builtin:gainPan");
	const Json limiter = insert(client, channel, "builtin:limiter");
	waitFor(client, gain, "active");
	waitFor(client, limiter, "active");
	setParameter(client, channel, gain, g_gain, -12.0);
	setParameter(client, channel, limiter, g_limiterCeiling, -20.0);
	CHECK_THAT(peakDb(client), WithinAbs(-20.0, 0.5)); // -18 dB, limited to -20 dB

	run(client, "plugin.move", {{"slot", gain}, {"index", 1}});
	CHECK_THAT(peakDb(client), WithinAbs(-32.0, 0.5)); // limited to -20 dB, then 12 dB less
}

TEST_CASE("a plugin moves with its slot to another channel, and a mono channel's plugin is mono", "[integration]")
{
	Engine engine;
	Client client(engine);
	const ObjectId channel = monitorSine(client, 500.0, -24.0);
	const ObjectId mono =
		run(client, "track.add", {{"type", "audio"}, {"format", "mono"}}).at("channel").at("id").get<ObjectId>();
	const Json slot = insert(client, channel, "builtin:eq");
	waitFor(client, slot, "active");
	setParameter(client, channel, slot, g_bellGain, 12.0);
	CHECK_THAT(peakDb(client), WithinAbs(-12.0, 0.1));

	run(client, "plugin.move", {{"slot", slot}, {"channel", mono}, {"index", 0}});
	waitFor(client, slot, "active"); // made again for one channel
	CHECK_THAT(peakDb(client), WithinAbs(-24.0, 0.01));
	CHECK(run(client, "plugin.get", {{"slot", slot}}).at("slot").at("parameters").size() == 1);
}

TEST_CASE("plugins in slots render the same with any number of processing threads", "[integration]")
{
	const auto render = [](int threads)
	{
		Engine engine;
		Client client(engine);
		std::vector<ObjectId> channels{monitorSine(client, 500.0, -12.0)};
		for (int t = 1; t < 4; ++t)
		{
			channels.push_back(run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>());
			run(client, "channel.set", {{"channel", channels.back()}, {"monitor", true}});
		}
		run(client, "audio.setProcessing", {{"threads", threads}});
		std::vector<Json> slots;
		for (size_t t = 0; t < channels.size(); ++t)
		{
			slots.push_back(insert(client, channels[t], "builtin:eq"));
			setParameter(client, channels[t], slots.back(), g_bellGain, 3.0 * static_cast<double>(t));
			slots.push_back(insert(client, channels[t], "builtin:limiter"));
			setParameter(client, channels[t], slots.back(), g_limiterCeiling, -6.0);
		}
		for (const Json& slot : slots)
			waitFor(client, slot, "active");
		run(client, "audio.render", {{"frames", g_block}});
		return run(client, "audio.capture", {{"channel", 0}, {"start", 0}, {"frames", g_block}}).at("samples");
	};
	const Json single = render(1);
	CHECK(single == render(4));
	CHECK(single.at(g_block - 1).get<double>() != 0.0);
}
