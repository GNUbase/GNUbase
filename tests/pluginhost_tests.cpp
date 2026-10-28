// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"
#include "engine/graph/graph.h"
#include "engine/plugins/hostmanager.h"
#include "engine/plugins/pluginslot.h"
#include "platform/process.h"
#include "plugin/extensions.h"
#include "plugin/remote/hostconnection.h"
#include "plugin/remote/protocol.h"
#include "plugin/remote/proxy.h"
#include "pluginhost/testplugins.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

// CMake passes the host's full path; the fallback only serves tools that compile this file on its own.
#ifndef GNUBASE_PLUGINHOST
#define GNUBASE_PLUGINHOST "gnubase-pluginhost"
#endif

using namespace gnubase;
using namespace gnubase::engine;
using namespace std::chrono_literals;

namespace
{
	constexpr double g_sampleRate = 48000.0;
	constexpr uint32_t g_maxFrames = 256;

	plugins::HostManager::Options testOptions()
	{
		plugins::HostManager::Options options;
		options.executable = GNUBASE_PLUGINHOST;
		return options;
	}

	plugins::PluginRef gain(const std::string& vendor = "GNUbase")
	{
		return {"test:gain", "", vendor};
	}

	// What an in-process plugin gets as its host.
	class LocalHost final : public plugin::Host
	{
	public:
		void requestRestart() override {}
		void requestProcess() override {}
		void requestCallback() override {}
		void* extension(std::string_view /*id*/) override { return nullptr; }
	};

	// An input bus, a plugin slot and an output bus, in a compiled graph.
	class SlotGraph
	{
	public:
		explicit SlotGraph(
			std::unique_ptr<plugin::Plugin> plugin, plugin::ProcessMode mode = plugin::ProcessMode::Offline)
			: m_slot(std::make_shared<plugins::PluginSlot>(std::move(plugin)))
		{
			m_slot->setProcessMode(mode);
			graph::GraphDesc desc;
			desc.nodes = {{graph::NodeKind::InputBus, 1, 2, nullptr}, {graph::NodeKind::PluginSlot, 2, 2, m_slot},
				{graph::NodeKind::OutputBus, 3, 2, nullptr}};
			desc.audio = {{0, 1}, {1, 2}};
			m_graph = std::make_unique<graph::CompiledGraph>(desc, g_sampleRate, g_maxFrames);
		}

		plugins::PluginSlot& slot() const { return *m_slot; }

		// Processes a block of `input` (both channels: input and its negative) with the parameter changes, and
		// returns the output's channels one after the other.
		std::vector<float> render(const std::vector<float>& input, const std::vector<graph::ProcessEvent>& events = {})
		{
			const auto frames = static_cast<uint32_t>(input.size());
			float* const* in = m_graph->audio(0);
			for (uint32_t i = 0; i < frames; ++i)
			{
				in[0][i] = input[i];
				in[1][i] = -input[i] * 0.5f;
			}
			for (const graph::ProcessEvent& event : events)
				m_graph->events(1).add(event);
			m_graph->process(frames);
			float* const* out = m_graph->audio(2);
			std::vector<float> output(out[0], out[0] + frames);
			output.insert(output.end(), out[1], out[1] + frames);
			return output;
		}

	private:
		std::shared_ptr<plugins::PluginSlot> m_slot;
		std::unique_ptr<graph::CompiledGraph> m_graph;
	};

	graph::ProcessEvent parameter(uint32_t offset, uint32_t id, double value)
	{
		graph::ProcessEvent event;
		event.offset = offset;
		event.type = graph::ProcessEvent::Type::Parameter;
		event.parameter = id;
		event.parameterValue = value;
		return event;
	}

	// Deterministic noise.
	class Noise
	{
	public:
		uint32_t next() { return m_state = (m_state * 1664525U) + 1013904223U; }
		float sample() { return (static_cast<float>(next() >> 8) / 8388608.0f) - 1.0f; }
		std::vector<float> block(uint32_t frames)
		{
			std::vector<float> samples(frames);
			for (float& sample : samples)
				sample = this->sample();
			return samples;
		}

	private:
		uint32_t m_state = 12345;
	};

	bool silent(const std::vector<float>& samples)
	{
		return std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0.0f; });
	}

	std::unique_ptr<plugin::remote::RemotePlugin> createRemote(
		plugins::HostManager& manager, const plugins::PluginRef& ref = gain())
	{
		std::unique_ptr<plugin::remote::RemotePlugin> remote = manager.create(ref);
		REQUIRE(remote->init());
		return remote;
	}

	std::vector<uint8_t> stateOf(plugin::Plugin& plugin)
	{
		std::vector<uint8_t> state;
		REQUIRE(plugin::extension<plugin::State>(plugin)->saveState(state, plugin::StateContext::Project));
		return state;
	}

	// Waits until the condition holds, for up to `timeout`.
	template <typename Condition> bool eventually(Condition condition, std::chrono::milliseconds timeout = 5000ms)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!condition())
		{
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(5ms);
		}
		return true;
	}
}

TEST_CASE("a test plugin processes audio through the host process sample-exactly", "[integration]")
{
	plugins::HostManager manager(testOptions());
	LocalHost host;
	std::unique_ptr<plugin::Plugin> local = pluginhost::createTestPlugin("test:gain", host);
	REQUIRE(local->init());
	SlotGraph inProcess(std::move(local));
	SlotGraph hosted(createRemote(manager));

	Noise noise;
	const uint32_t sizes[] = {256, 1, 100, 255, 64, 17, 256};
	bool changed = false;
	for (int block = 0; block < 60; ++block)
	{
		const uint32_t frames = sizes[static_cast<size_t>(block) % std::size(sizes)];
		const std::vector<float> input = noise.block(frames);
		// Parameter changes at random frames: the gain from -30 to +12 dB and the polarity.
		std::vector<graph::ProcessEvent> events;
		for (uint32_t i = 0; i < noise.next() % 4; ++i)
		{
			const uint32_t offset = noise.next() % frames;
			events.push_back(noise.next() % 2 == 0
					? parameter(offset, pluginhost::Gain, -30.0 + static_cast<double>(noise.next() % 43))
					: parameter(offset, pluginhost::Invert, static_cast<double>(noise.next() % 2)));
		}
		std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
		changed = changed || !events.empty();

		const std::vector<float> expected = inProcess.render(input, events);
		const std::vector<float> actual = hosted.render(input, events);
		REQUIRE(actual == expected);
		REQUIRE(!hosted.slot().failed());
	}
	CHECK(changed);

	// The engine mirrors the parameter values the plugin got.
	double fromMirror = 0.0;
	double fromPlugin = 1.0;
	REQUIRE(plugin::extension<plugin::Params>(hosted.slot().plugin())->paramValue(pluginhost::Gain, fromMirror));
	REQUIRE(plugin::extension<plugin::Params>(inProcess.slot().plugin())->paramValue(pluginhost::Gain, fromPlugin));
	CHECK(fromMirror == fromPlugin);
}

TEST_CASE("the host process answers control calls and exits when the engine closes it", "[integration]")
{
	plugins::HostManager manager(testOptions());
	uint32_t process = 0;
	{
		const std::unique_ptr<plugin::remote::RemotePlugin> remote = createRemote(manager);
		process = remote->host()->processId();
		CHECK(remote->descriptor().name == "Test Gain");
		CHECK(remote->descriptor().vendor == "GNUbase");
		CHECK(remote->audioPortCount(true) == 1);
		CHECK(remote->paramCount() == 2);
		CHECK(remote->extension("gnubase.gui") == nullptr);

		std::string text;
		REQUIRE(remote->paramValueToText(pluginhost::Gain, -6.0, text));
		CHECK(text == "-6.0 dB");
		double value = 0.0;
		REQUIRE(remote->paramTextToValue(pluginhost::Invert, "On", value));
		CHECK(value == 1.0);

		// Changes while the plugin doesn't process go through flushParams on the main thread.
		plugin::EventList in(4);
		plugin::EventList out(4);
		plugin::Event event;
		event.type = plugin::EventType::ParamValue;
		event.param = pluginhost::Gain;
		event.value = -12.0;
		in.push(event);
		remote->flushParams(in, out);
		REQUIRE(remote->paramValue(pluginhost::Gain, value));
		CHECK(value == -12.0);

		// State saved in the host loads into another instance.
		const std::vector<uint8_t> state = stateOf(*remote);
		const std::unique_ptr<plugin::remote::RemotePlugin> other = createRemote(manager);
		REQUIRE(other->loadState(state.data(), state.size(), plugin::StateContext::Project));
		REQUIRE(other->paramValue(pluginhost::Gain, value));
		CHECK(value == -12.0);
		CHECK(stateOf(*other) == state);
		CHECK(manager.hosts().size() == 1);
	}
	// The last plugin of the process is gone, so the manager closed the connection and the process exited.
	CHECK(manager.hosts().empty());
	CHECK(eventually([&] { return !platform::processExists(process); }));
}

TEST_CASE("the host process refuses an engine with another protocol version", "[integration]")
{
	plugin::remote::LaunchOptions options;
	options.executable = GNUBASE_PLUGINHOST;
	options.protocolVersion = plugin::remote::g_protocolVersion + 1;
	CHECK_THROWS_AS(plugin::remote::HostConnection::launch(options), plugin::remote::LaunchError);

	options.protocolVersion = plugin::remote::g_protocolVersion;
	const std::shared_ptr<plugin::remote::HostConnection> host = plugin::remote::HostConnection::launch(options);
	CHECK(host->processId() != 0);
	CHECK(!host->exited());
}

TEST_CASE("switching the hosting mode keeps every plugin's state and sound", "[integration]")
{
	plugins::HostManager manager(testOptions());
	REQUIRE(manager.mode() == plugins::g_defaultHostingMode);
	REQUIRE(manager.mode() == plugins::HostingMode::PerVendor);

	// Three plugins with settings of their own, from two vendors.
	std::vector<std::unique_ptr<SlotGraph>> graphs;
	graphs.push_back(std::make_unique<SlotGraph>(createRemote(manager, gain("GNUbase"))));
	graphs.push_back(std::make_unique<SlotGraph>(createRemote(manager, gain("GNUbase"))));
	graphs.push_back(std::make_unique<SlotGraph>(createRemote(manager, gain("Other"))));
	Noise noise;
	const std::vector<float> input = noise.block(g_maxFrames);
	const double gains[] = {-6.0, 3.0, -12.0};
	std::vector<std::vector<float>> expected;
	std::vector<std::vector<uint8_t>> states;
	for (size_t i = 0; i < graphs.size(); ++i)
	{
		graphs[i]->render(input,
			{parameter(0, pluginhost::Gain, gains[i]), parameter(0, pluginhost::Invert, static_cast<double>(i % 2))});
		expected.push_back(graphs[i]->render(input));
		states.push_back(stateOf(graphs[i]->slot().plugin()));
	}
	CHECK(manager.hosts().size() == 2);

	const auto check = [&](size_t processes)
	{
		const std::vector<plugins::HostManager::HostStatus> hosts = manager.hosts();
		CHECK(hosts.size() == processes);
		for (size_t i = 0; i < graphs.size(); ++i)
		{
			CHECK(graphs[i]->render(input) == expected[i]);
			CHECK(stateOf(graphs[i]->slot().plugin()) == states[i]);
			double value = 0.0;
			CHECK(plugin::extension<plugin::Params>(graphs[i]->slot().plugin())->paramValue(pluginhost::Gain, value));
			CHECK(value == gains[i]);
		}
	};

	CHECK(manager.setMode(plugins::HostingMode::PerInstance).empty());
	check(3);
	CHECK(manager.setMode(plugins::HostingMode::Shared).empty());
	check(1);
	// A plugin with an override of its own.
	CHECK(manager.setOverride("test:gain", plugins::HostingMode::PerInstance).empty());
	check(3);
	CHECK(manager.setOverride("test:gain", std::nullopt).empty());
	check(1);
	CHECK(manager.setMode(plugins::HostingMode::PerPlugin).empty());
	check(1);
	CHECK(manager.setMode(plugins::HostingMode::PerVendor).empty());
	check(2);
}

TEST_CASE("a plugin moves to another host process while it processes, without a gap", "[integration]")
{
	plugins::HostManager manager(testOptions());
	SlotGraph graph(createRemote(manager));
	Noise noise;
	const std::vector<float> input = noise.block(64);
	graph.render(input, {parameter(0, pluginhost::Gain, -6.0)});
	const std::vector<float> expected = graph.render(input);

	// The audio thread renders on while the command thread switches the hosting mode back and forth.
	std::atomic<bool> stop{false};
	std::atomic<int> blocks{0};
	std::atomic<int> wrong{0};
	std::thread audio(
		[&]
		{
			while (!stop)
			{
				if (graph.render(input) != expected)
					++wrong;
				++blocks;
			}
		});
	std::vector<uint32_t> processes;
	for (int i = 0; i < 6; ++i)
	{
		CHECK(manager.setMode(i % 2 == 0 ? plugins::HostingMode::PerInstance : plugins::HostingMode::Shared).empty());
		processes.push_back(manager.hosts().at(0).process);
	}
	const int before = blocks;
	CHECK(eventually([&] { return blocks > before + 10; }));
	stop = true;
	audio.join();
	CHECK(wrong == 0);
	CHECK(processes[0] != processes[1]); // it did move
}

TEST_CASE("killing the host process mid-render yields silence and an error event", "[integration]")
{
	Engine engine;
	Client client(engine);
	plugins::HostManager& manager = engine.pluginHosts();
	std::unique_ptr<plugin::remote::RemotePlugin> remote = manager.create(gain());
	REQUIRE(remote->init());
	const uint64_t instance = manager.instanceOf(*remote);
	const uint32_t process = remote->host()->processId();
	SlotGraph graph(std::move(remote));
	Noise noise;
	const std::vector<float> input = noise.block(128);

	// The audio thread renders while the host process is killed.
	std::atomic<bool> stop{false};
	std::atomic<int> silentBlocks{0};
	std::atomic<int> blocks{0};
	std::thread audio(
		[&]
		{
			while (!stop)
			{
				silentBlocks += silent(graph.render(input)) ? 1 : 0;
				++blocks;
				std::this_thread::sleep_for(1ms);
			}
		});
	CHECK(eventually([&] { return blocks > 20; }));
	CHECK(silentBlocks == 0);
	REQUIRE(manager.kill(process));

	// The plugin is bypassed, and the engine reports it.
	bool reported = false;
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (!reported && std::chrono::steady_clock::now() < deadline)
	{
		const std::optional<Message> message = client.wait(100ms);
		const auto* event = message ? std::get_if<Event>(&*message) : nullptr;
		if (event != nullptr && event->name == "plugin.failed")
		{
			CHECK(event->data.at("instance").get<uint64_t>() == instance);
			CHECK(event->data.at("plugin") == "test:gain");
			CHECK(event->data.at("reason") == "crashed");
			reported = true;
		}
	}
	CHECK(reported);
	const int silentBefore = silentBlocks;
	const int blocksBefore = blocks;
	CHECK(eventually([&] { return blocks > blocksBefore + 20; }));
	stop = true;
	audio.join();
	CHECK(graph.slot().failed());
	CHECK(silentBlocks - silentBefore == blocks - blocksBefore); // only silence since

	// GNUbase keeps running.
	const Result version = client.call("engine.version");
	CHECK(version.ok());
	const Result hosts = client.call("pluginhost.get");
	REQUIRE(hosts.ok());
	REQUIRE(hosts.value.at("hosts").size() == 1);
	CHECK(hosts.value.at("hosts")[0].at("state") == "exited");
	CHECK(hosts.value.at("hosts")[0].at("instances")[0].at("failed") == true);
}

TEST_CASE("a hung host process is detected within the deadline", "[integration]")
{
	plugins::HostManager::Options options = testOptions();
	options.deadlines.realtime = 300ms;
	plugins::HostManager manager(options);
	SlotGraph graph(createRemote(manager, {"test:hang", "", "GNUbase"}), plugin::ProcessMode::Realtime);
	Noise noise;
	const std::vector<float> input = noise.block(128);

	const auto start = std::chrono::steady_clock::now();
	const std::vector<float> output = graph.render(input);
	const auto elapsed = std::chrono::steady_clock::now() - start;
	CHECK(elapsed >= 300ms);
	CHECK(elapsed < 1300ms);
	CHECK(silent(output));
	CHECK(graph.slot().failed());

	// Later blocks don't wait for it any more.
	const auto next = std::chrono::steady_clock::now();
	CHECK(silent(graph.render(input)));
	CHECK(std::chrono::steady_clock::now() - next < 100ms);

	std::vector<plugins::PluginFailure> failures;
	CHECK(eventually(
		[&]
		{
			failures = manager.takeFailures();
			return !failures.empty();
		}));
	REQUIRE(failures.size() == 1);
	CHECK(failures[0].plugin == "test:hang");
	CHECK(failures[0].reason == "hung");
	CHECK(manager.hosts().at(0).state == "hung");
}

TEST_CASE("the hosting mode is set through engine commands", "[integration]")
{
	Engine engine;
	Client client(engine);
	Result result = client.call("pluginhost.get");
	REQUIRE(result.ok());
	CHECK(result.value.at("mode") == "perVendor");
	CHECK(result.value.at("defaultMode") == "perVendor");
	CHECK(result.value.at("hosts").empty());

	result = client.call("pluginhost.setMode", {{"mode", "perInstance"}});
	REQUIRE(result.ok());
	CHECK(result.value.at("mode") == "perInstance");
	CHECK(engine.pluginHosts().mode() == plugins::HostingMode::PerInstance);
	CHECK(!client.call("pluginhost.setMode", {{"mode", "inProcess"}}).ok()); // there is no in-process mode

	result = client.call("pluginhost.setOverride", {{"plugin", "clap:com.example.synth"}, {"mode", "shared"}});
	REQUIRE(result.ok());
	CHECK(result.value.at("overrides").at("clap:com.example.synth") == "shared");
	result = client.call("pluginhost.setOverride", {{"plugin", "clap:com.example.synth"}, {"mode", nullptr}});
	REQUIRE(result.ok());
	CHECK(result.value.at("overrides").empty());

	result = client.call("pluginhost.check");
	REQUIRE(result.ok());
	CHECK(result.value.at("failures").empty());
}

TEST_CASE("the plugin protocol's reader never reads past its data", "[unit]")
{
	plugin::remote::Writer writer;
	writer.string("test:gain");
	writer.u32(7);
	std::vector<uint8_t> data = writer.data();
	{
		plugin::remote::Reader reader(data);
		CHECK(reader.string() == "test:gain");
		CHECK(reader.u32() == 7);
		CHECK(reader.ok());
		CHECK(reader.u8() == 0);
		CHECK(!reader.ok());
	}
	// A size larger than the data.
	data[0] = 0xff;
	plugin::remote::Reader reader(data);
	CHECK(reader.string().empty());
	CHECK(!reader.ok());
	plugin::remote::PluginInfo info;
	plugin::remote::Reader truncated(data.data(), 3);
	CHECK(!plugin::remote::read(truncated, info));
}

TEST_CASE("plugin hosts find the engine's process and their program", "[unit]")
{
	CHECK(platform::processExists(platform::currentProcessId()));
	CHECK(platform::currentExecutablePath().filename().string().find("gnubase-tests") == 0);
	platform::ChildProcess child = platform::ChildProcess::start(GNUBASE_PLUGINHOST, {});
	const uint32_t id = child.id();
	CHECK(child.wait(5000ms) == 2); // no connection given
	CHECK(!platform::processExists(id));
}
