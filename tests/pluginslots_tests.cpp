// SPDX-License-Identifier: GPL-3.0-or-later
// Plugins in slots in the audio engine (#282): a channel's audio passes its slots from top to bottom before the fader,
// a bypassed or inactive plugin passes it unchanged, and the engine makes, activates and destroys the plugins as the
// slots change.
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/plugins/hostmanager.h"
#include "engine/plugins/pluginslot.h"
#include "engine/plugins/slotinstances.h"
#include "engine/processors/builtinplugin.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
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

	class TestHost final : public gnubase::plugin::Host
	{
	public:
		void requestRestart() override {}
		void requestProcess() override {}
		void requestCallback() override {}
		void* extension(std::string_view /*id*/) override { return nullptr; }
	};

	// A plugin with parameters 0 to count - 1 that keeps the last value each got in a block; one that doesn't
	// initialize, if asked. It has no audio ports.
	class CountingPlugin final : public gnubase::plugin::Plugin, public gnubase::plugin::Params
	{
	public:
		CountingPlugin(uint32_t count, bool initializes) : values(count, -1.0), m_initializes(initializes)
		{
			m_descriptor.id = "test:counting";
		}

		std::vector<double> values; // -1 until it gets one
		std::atomic<size_t> changes{0};
		std::array<gnubase::plugin::TransportSegment, 4> transport{}; // of the last block
		uint32_t transportCount = 0;
		gnubase::plugin::ProcessMode mode = gnubase::plugin::ProcessMode::Prefetch; // of the last block

		const gnubase::plugin::Descriptor& descriptor() const override { return m_descriptor; }
		bool init() override { return m_initializes; }
		bool activate(double /*sampleRate*/, uint32_t /*minFrames*/, uint32_t /*maxFrames*/) override { return true; }
		void deactivate() override {}
		bool startProcessing() noexcept override { return true; }
		void stopProcessing() noexcept override {}
		void reset() noexcept override {}
		gnubase::plugin::ProcessStatus process(const gnubase::plugin::Process& process) noexcept override
		{
			mode = process.mode;
			transportCount = std::min(process.transportCount, static_cast<uint32_t>(transport.size()));
			std::copy_n(process.transport, transportCount, transport.begin());
			for (const gnubase::plugin::Event& event : process.eventsIn)
				if (event.type == gnubase::plugin::EventType::ParamValue && event.param < values.size())
				{
					values[event.param] = event.value;
					++changes;
				}
			return gnubase::plugin::ProcessStatus::Continue;
		}
		void* extension(std::string_view id) override
		{
			return gnubase::plugin::provide<gnubase::plugin::Params>(this, id);
		}

		uint32_t paramCount() const override { return static_cast<uint32_t>(values.size()); }
		bool paramInfo(uint32_t index, gnubase::plugin::ParamInfo& info) const override
		{
			if (index >= values.size())
				return false;
			info.id = index;
			info.name = "Parameter " + std::to_string(index);
			return true;
		}
		bool paramValue(uint32_t param, double& value) const override
		{
			if (param >= values.size())
				return false;
			value = values[param];
			return true;
		}
		bool paramValueToText(uint32_t /*param*/, double /*value*/, std::string& /*text*/) const override
		{
			return false;
		}
		bool paramTextToValue(uint32_t /*param*/, std::string_view /*text*/, double& /*value*/) const override
		{
			return false;
		}
		void flushParams(
			const gnubase::plugin::EventList& /*in*/, gnubase::plugin::EventList& /*out*/) noexcept override
		{
		}

	private:
		gnubase::plugin::Descriptor m_descriptor;
		bool m_initializes = true;
	};

	// Processes a stereo block of 128 frames of the samples.
	void processBlock(plugins::PluginSlot& slot, std::vector<float>& left, std::vector<float>& right)
	{
		const graph::EventBuffer eventsIn(4);
		graph::EventBuffer eventsOut(4);
		const graph::TransportBlock transport;
		const std::array<float*, 2> audio{left.data(), right.data()};
		slot.process({audio.data(), 2, static_cast<uint32_t>(left.size()), eventsIn, eventsOut, transport});
	}

	// The data of the events since the last call, by name.
	std::map<std::string, std::vector<Json>> events(Client& client)
	{
		std::map<std::string, std::vector<Json>> found;
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<Event>(&*message))
				found[event->name].push_back(event->data);
		return found;
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
	TestHost host;
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

TEST_CASE("bypassing a plugin with latency delays the input by as much, so the timing stays", "[unit]")
{
	TestHost host;
	std::unique_ptr<gnubase::plugin::Plugin> limiter = processors::BuiltinFactory().create("builtin:limiter", host);
	REQUIRE(limiter->init());
	plugins::PluginSlot slot(std::move(limiter));
	slot.setParameters({{g_limiterCeiling, 0.0}}); // far above the input: it only delays it
	REQUIRE(slot.activate(48000.0, 128));
	const uint32_t latency = slot.latency();
	REQUIRE(latency == 48);

	std::vector<float> input;
	std::vector<float> played;
	std::vector<float> left(128);
	std::vector<float> right(128);
	for (int block = 0; block < 40; ++block)
	{
		slot.setBypass(block >= 30 || (block >= 10 && block < 20));
		for (size_t i = 0; i < left.size(); ++i)
		{
			const double t = static_cast<double>(input.size()) / 48000.0;
			left[i] = static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979 * 440.0 * t));
			right[i] = left[i];
			input.push_back(left[i]);
		}
		processBlock(slot, left, right);
		played.insert(played.end(), left.begin(), left.end());
		CHECK(slot.latency() == latency);
	}
	float largest = 0.0f;
	for (size_t i = latency; i < played.size(); ++i)
		largest = std::max(largest, std::abs(played[i] - input[i - latency]));
	CHECK(largest < 1e-3f); // 0.23 where the undelayed input comes in
}

TEST_CASE("a plugin sees which segments of a block record", "[unit]")
{
	auto made = std::make_unique<CountingPlugin>(0, true);
	CountingPlugin& plugin = *made;
	plugins::PluginSlot slot(std::move(made));
	REQUIRE(slot.activate(48000.0, 128));
	const std::array<graph::TransportSegment, 2> segments{{{0, 64, 1000, false}, {64, 64, 1064, true}}};
	graph::TransportBlock transport;
	transport.playing = true;
	transport.recording = true;
	transport.position = 1000;
	transport.segments = segments.data();
	transport.segmentCount = segments.size();
	const graph::EventBuffer eventsIn(4);
	graph::EventBuffer eventsOut(4);
	std::vector<float> left(128);
	std::vector<float> right(128);
	const std::array<float*, 2> audio{left.data(), right.data()};
	slot.process({audio.data(), 2, 128, eventsIn, eventsOut, transport});
	REQUIRE(plugin.transportCount == 2);
	using gnubase::plugin::Transport;
	CHECK(plugin.transport[0].transport.flags == Transport::Playing);
	CHECK(plugin.transport[1].transport.flags == (Transport::Playing | Transport::Recording));
	CHECK(plugin.transport[1].offset == 64);
	CHECK(plugin.transport[1].transport.position == 1064);
}

TEST_CASE("a built-in processor behind the plugin wrapper sees which segments record", "[unit]")
{
	std::array<gnubase::plugin::TransportSegment, 2> transport{};
	using gnubase::plugin::Transport;
	transport[0].transport.flags = Transport::Playing;
	transport[0].transport.position = 1000;
	transport[1].offset = 64;
	transport[1].transport.flags = Transport::Playing | Transport::Recording;
	transport[1].transport.position = 1064;
	const gnubase::plugin::EventList in(4);
	gnubase::plugin::EventList out(4);
	const gnubase::plugin::Process process{
		128, -1, gnubase::plugin::ProcessMode::Realtime, nullptr, 0, nullptr, 0, in, out, transport.data(), 2};
	std::vector<graph::TransportSegment> segments(4);
	const graph::TransportBlock block = processors::toTransportBlock(process, segments);
	CHECK(block.playing);
	CHECK(block.recording);
	REQUIRE(block.segmentCount == 2);
	CHECK(!block.segments[0].recording);
	CHECK(block.segments[1].recording);
	CHECK(block.segments[1].offset == 64);
	CHECK(block.segments[1].frames == 64);
	CHECK(block.segments[1].position == 1064);
}

TEST_CASE("more parameter changes than a block has room for reach the plugin in the next blocks", "[unit]")
{
	constexpr uint32_t count = 3000; // almost three times graph::g_maxEventsPerBlock
	auto made = std::make_unique<CountingPlugin>(count, true);
	CountingPlugin& plugin = *made;
	plugins::PluginSlot slot(std::move(made));
	REQUIRE(slot.activate(48000.0, 128));
	std::map<uint32_t, double> values;
	for (uint32_t i = 0; i < count; ++i)
		values[i] = 0.5;
	slot.setParameters(values);

	std::vector<float> left(128);
	std::vector<float> right(128);
	processBlock(slot, left, right);
	CHECK(plugin.changes > 0);
	CHECK(plugin.changes <= graph::g_maxEventsPerBlock / 2); // room for the block's own events
	for (int block = 0; block < 10; ++block)
		processBlock(slot, left, right);
	CHECK(plugin.changes == count);
	CHECK(std::all_of(plugin.values.begin(), plugin.values.end(), [](double value) { return value == 0.5; }));

	// Later changes come once.
	values[7] = 0.25;
	slot.setParameters(values);
	processBlock(slot, left, right);
	processBlock(slot, left, right);
	CHECK(plugin.changes == count + 1);
	CHECK(plugin.values[7] == 0.25);
}

TEST_CASE("a slot whose plugin can't be made is unavailable, and one whose plugin doesn't initialize failed", "[unit]")
{
	plugins::HostManager jobs;
	std::atomic<int> changed{0};
	plugins::SlotInstances slots(
		jobs, [&changed] { ++changed; }, [](uint64_t /*generation*/) { return true; },
		[](const std::string& plugin, uint32_t /*channels*/,
			gnubase::plugin::Host& /*host*/) -> std::unique_ptr<gnubase::plugin::Plugin>
		{
			if (plugin == "test:broken")
				return std::make_unique<CountingPlugin>(1, false);
			if (plugin == "test:working")
				return std::make_unique<CountingPlugin>(1, true);
			return nullptr;
		});
	RenderSlot missing;
	missing.id = 1;
	missing.plugin = "test:missing";
	RenderSlot broken;
	broken.id = 2;
	broken.plugin = "test:broken";
	RenderSlot working;
	working.id = 3;
	working.plugin = "test:working";
	const std::vector<plugins::SlotInstances::Wanted> wanted{{&missing, 2}, {&broken, 2}, {&working, 2}};
	slots.update(wanted, 48000.0, 128);
	jobs.run([] {}).get(); // after the jobs that make them
	CHECK(slots.status(1) == "unavailable");
	CHECK(slots.status(2) == "failed");
	CHECK(slots.processor(2) == nullptr);
	CHECK(slots.status(3) == "active");
	CHECK(changed == 3);

	// Neither is tried again until the slot changes.
	slots.update(wanted, 48000.0, 128);
	slots.retire(1);
	jobs.run([] {}).get();
	CHECK(changed == 3);
	CHECK(slots.status(1) == "unavailable");
	CHECK(slots.processor(3) != nullptr);
	CHECK(slots.status(4).empty());
}

TEST_CASE("the slots' plugins get offline blocks while the engine renders offline, e.g. an export", "[unit]")
{
	plugins::HostManager jobs;
	CountingPlugin* made = nullptr;
	plugins::SlotInstances slots(
		jobs, [] {}, [](uint64_t /*generation*/) { return true; },
		[&made](const std::string& /*plugin*/, uint32_t /*channels*/,
			gnubase::plugin::Host& /*host*/) -> std::unique_ptr<gnubase::plugin::Plugin>
		{
			auto plugin = std::make_unique<CountingPlugin>(1, true);
			made = plugin.get();
			return plugin;
		});
	RenderSlot slot;
	slot.id = 1;
	slot.plugin = "test:counting";
	const std::vector<plugins::SlotInstances::Wanted> wanted{{&slot, 2}};
	slots.update(wanted, 48000.0, 128);
	jobs.run([] {}).get();
	slots.update(wanted, 48000.0, 128); // in the graph now
	const std::shared_ptr<plugins::PluginSlot> processor = slots.processor(1);
	REQUIRE(processor != nullptr);
	REQUIRE(made != nullptr);
	std::vector<float> left(128);
	std::vector<float> right(128);
	processBlock(*processor, left, right);
	CHECK(made->mode == gnubase::plugin::ProcessMode::Realtime);
	slots.setProcessMode(gnubase::plugin::ProcessMode::Offline);
	processBlock(*processor, left, right);
	CHECK(made->mode == gnubase::plugin::ProcessMode::Offline);
	slots.setProcessMode(gnubase::plugin::ProcessMode::Realtime);
	processBlock(*processor, left, right);
	CHECK(made->mode == gnubase::plugin::ProcessMode::Realtime);
}

TEST_CASE("the engine closes while plugins in slots are made and changed", "[integration]")
{
	for (int i = 0; i < 5; ++i)
	{
		Engine engine;
		Client client(engine);
		const ObjectId channel = monitorSine(client, 500.0, -24.0);
		run(client, "audio.start");
		insert(client, channel, "builtin:eq");
		const Json limiter = insert(client, channel, "builtin:limiter");
		run(client, "plugin.set", {{"slot", limiter}, {"active", false}});
		// The engine goes now, with the plugins' jobs queued.
	}
	SUCCEED();
}

TEST_CASE("plugin.changed announces a slot once its plugin's status changed", "[integration]")
{
	Engine engine;
	Client client(engine);
	const ObjectId channel = monitorSine(client, 500.0, -24.0);
	const Json slot = insert(client, channel, "builtin:eq");
	waitFor(client, slot, "active");
	run(client, "plugin.set", {{"slot", slot}, {"active", false}});
	waitFor(client, slot, "inactive");
	run(client, "audio.updatePlugins"); // announces nothing new

	const std::vector<Json> announced = events(client)["plugin.changed"];
	std::vector<std::string> statuses;
	statuses.reserve(announced.size());
	for (const Json& changed : announced)
		statuses.push_back(changed.at("slot").at("status").get<std::string>());
	REQUIRE(!statuses.empty());
	CHECK(std::count(statuses.begin(), statuses.end(), "active") >= 1);
	CHECK(std::count(statuses.begin(), statuses.end(), "inactive") == 1);
	CHECK(statuses.back() == "inactive");
}
