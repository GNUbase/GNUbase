// SPDX-License-Identifier: GPL-3.0-or-later
// Plugins in slots in the audio engine (#282): a channel's audio passes its slots from top to bottom before the fader,
// a bypassed or inactive plugin passes it unchanged, and the engine makes, activates and destroys the plugins as the
// slots change.
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/plugins/pluginslot.h"
#include "engine/processors/builtinplugin.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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

TEST_CASE("switching bypass crossfades over a block, so it doesn't click", "[unit]")
{
	class NullHost final : public gnubase::plugin::Host
	{
	public:
		void requestRestart() override {}
		void requestProcess() override {}
		void requestCallback() override {}
		void* extension(std::string_view /*id*/) override { return nullptr; }
	};
	NullHost host;
	std::unique_ptr<gnubase::plugin::Plugin> gain = processors::BuiltinFactory().create("builtin:gainPan", host);
	REQUIRE(gain->init());
	plugins::PluginSlot slot(std::move(gain));
	slot.setParameters({{g_gain, -12.0}});
	REQUIRE(slot.activate(48000.0, 128));

	std::vector<float> left;
	std::vector<float> right;
	const graph::EventBuffer eventsIn(4);
	graph::EventBuffer eventsOut(4);
	const graph::TransportBlock transport;
	std::vector<float> played;
	for (int block = 0; block < 40; ++block)
	{
		if (block == 20)
			slot.setBypass(true);
		left.assign(128, 0.5f);
		right.assign(128, 0.5f);
		const std::array<float*, 2> audio{left.data(), right.data()};
		slot.process({audio.data(), 2, 128, eventsIn, eventsOut, transport});
		played.insert(played.end(), left.begin(), left.end());
	}
	CHECK_THAT(played[(20 * 128) - 1], WithinAbs(0.5 * std::pow(10.0, -12.0 / 20.0), 1e-4));
	CHECK(played.back() == 0.5f);
	float largestStep = 0.0f;
	for (size_t i = 1; i < played.size(); ++i)
		largestStep = std::max(largestStep, std::abs(played[i] - played[i - 1]));
	CHECK(largestStep < 0.01f); // 0.37 at once without the crossfade
}

TEST_CASE("plugins change while the device plays, and a new sample rate activates them again", "[integration]")
{
	Engine engine;
	Client client(engine);
	const ObjectId channel = monitorSine(client, 500.0, -24.0);
	const ObjectId other = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
	run(client, "audio.start");
	const Json limiter = insert(client, channel, "builtin:limiter");
	const Json eq = insert(client, other, "builtin:eq");
	waitFor(client, limiter, "active");
	waitFor(client, eq, "active");

	const auto node = [&client](const Json& object)
	{
		const Json graph = run(client, "graph.get");
		for (const Json& n : graph.at("nodes"))
			if (n.at("object") == object)
				return n;
		return Json();
	};
	CHECK(node(limiter).at("kind") == "pluginSlot");

	// Deactivating, bypassing and removing while blocks are processed: the plugin leaves the graph first.
	run(client, "plugin.set", {{"slot", limiter}, {"active", false}});
	waitFor(client, limiter, "inactive");
	run(client, "plugin.set", {{"slot", limiter}, {"active", true}, {"bypass", true}});
	waitFor(client, limiter, "active");
	run(client, "plugin.remove", {{"slot", eq}});
	run(client, "history.undo");
	waitFor(client, eq, "active");
	// A new sample rate activates them again.
	run(client, "audio.stop");
	run(client, "audio.setDevice", {{"sampleRate", 44100.0}});
	run(client, "audio.start");
	waitFor(client, limiter, "active");
	waitFor(client, eq, "active");
	run(client, "audio.stop");
	CHECK(run(client, "plugin.get", {{"slot", limiter}}).at("slot").at("bypass") == true);
}
