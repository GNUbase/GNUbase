// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"
#include "engine/graph/graph.h"
#include "engine/plugins/hostmanager.h"
#include "engine/plugins/pluginslot.h"
#include "ipc/connection.h"
#include "platform/process.h"
#include "plugin/extensions.h"
#include "plugin/remote/hostconnection.h"
#include "plugin/remote/protocol.h"
#include "plugin/remote/proxy.h"
#include "pluginhost/testformat.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

// CMake passes the tests' host's full path (tests/pluginhost); the fallback only serves tools that compile this file on
// its own.
#ifndef GNUBASE_PLUGINHOST
#define GNUBASE_PLUGINHOST "gnubase-testpluginhost"
#endif

using namespace gnubase;
using namespace gnubase::engine;
using namespace std::chrono_literals;

namespace
{
	constexpr double g_sampleRate = 48000.0;
	constexpr uint32_t g_maxFrames = 256;

	// Parameter ids of the test plugins (tests/plugins/README.md).
	constexpr uint32_t g_gain = 1;      // test:gain, dB
	constexpr uint32_t g_hang = 1;      // test:hang, 0 or 1
	constexpr uint32_t g_latency = 1;   // test:latency, samples
	constexpr uint32_t g_seed = 1;      // test:stateblob
	constexpr uint32_t g_stateSize = 2; // test:stateblob, bytes

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

	// The engine's side of what a remote plugin asks for: counts the calls.
	class EngineHost final : public plugin::Host, public plugin::HostLatency, public plugin::HostParams
	{
	public:
		void requestRestart() override { ++restarts; }
		void requestProcess() override {}
		void requestCallback() override {}
		void* extension(std::string_view id) override
		{
			return plugin::provide<plugin::HostLatency, plugin::HostParams>(this, id);
		}
		void latencyChanged() override { ++latencyChanges; }
		void rescanParams(uint32_t what) override
		{
			if ((what & plugin::HostParams::Values) != 0)
				++valueRescans;
		}
		void requestParamFlush() override {}

		std::atomic<int> restarts{0};
		std::atomic<int> latencyChanges{0};
		std::atomic<int> valueRescans{0};
	};

	std::unique_ptr<plugin::Plugin> localPlugin(const std::string& id, plugin::Host& host)
	{
		std::unique_ptr<plugin::Plugin> local = testformat::createTestPlugin(id, host);
		REQUIRE(local);
		REQUIRE(local->init());
		return local;
	}

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

	plugin::Event paramEvent(uint32_t id, double value)
	{
		plugin::Event event;
		event.type = plugin::EventType::ParamValue;
		event.param = id;
		event.value = value;
		return event;
	}

	// Sets parameters through flushParams.
	void flush(plugin::Plugin& plugin, const std::vector<plugin::Event>& events)
	{
		plugin::EventList in(events.size() + 1);
		for (const plugin::Event& event : events)
			in.push(event);
		plugin::EventList out(16);
		plugin::extension<plugin::Params>(plugin)->flushParams(in, out);
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

	double valueOf(plugin::Plugin& plugin, uint32_t param)
	{
		double value = std::numeric_limits<double>::quiet_NaN();
		CHECK(plugin::extension<plugin::Params>(plugin)->paramValue(param, value));
		return value;
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

	// A raw block to a plugin instance's audio connection, as the proxy sends it, and the status of its answer.
	struct RawBlock
	{
		uint32_t frames = 0;
		uint32_t flags = 0;
		uint32_t transportCount = 0;          // what ProcessInfo says
		std::vector<uint32_t> segmentOffsets; // the transport segments sent
		std::vector<plugin::Event> events;
	};

	std::optional<plugin::ProcessStatus> exchangeRaw(ipc::Connection& audio, uint64_t sequence, const RawBlock& raw)
	{
		ipc::AudioBlock block;
		REQUIRE(audio.beginBlock(block));
		plugin::remote::ProcessInfo info;
		info.flags = raw.flags;
		info.transportCount = static_cast<uint16_t>(raw.transportCount);
		const auto version = plugin::remote::g_messageVersion;
		REQUIRE(audio.sendEvent(
			static_cast<uint16_t>(plugin::remote::AudioMessage::ProcessInfo), version, &info, sizeof(info)));
		for (const uint32_t offset : raw.segmentOffsets)
		{
			plugin::TransportSegment segment;
			segment.offset = offset;
			REQUIRE(audio.sendEvent(
				static_cast<uint16_t>(plugin::remote::AudioMessage::Transport), version, &segment, sizeof(segment)));
		}
		for (const plugin::Event& event : raw.events)
			REQUIRE(audio.sendEvent(
				static_cast<uint16_t>(plugin::remote::AudioMessage::Event), version, &event, sizeof(event)));
		block.sequence = sequence;
		block.frames = raw.frames;
		block.channels = raw.frames > 0 ? 2 : 0;
		block.eventCount = static_cast<uint32_t>(1 + raw.segmentOffsets.size() + raw.events.size());
		audio.sendBlock(block);

		if (audio.waitForAnswer(sequence, 5s) != ipc::WaitResult::Ready)
			return std::nullopt;
		ipc::AudioBlock answer;
		REQUIRE(audio.receiveBlock(answer) == ipc::ReadStatus::Ok);
		std::optional<plugin::ProcessStatus> status;
		std::vector<std::byte> message(plugin::remote::g_maxAudioMessage);
		for (uint32_t i = 0; i < answer.eventCount; ++i)
		{
			ipc::MessageHeader header;
			REQUIRE(audio.receiveEvent(header, message.data(), message.size()) == ipc::ReadStatus::Ok);
			if (header.type == static_cast<uint16_t>(plugin::remote::AudioMessage::Result))
			{
				plugin::remote::ProcessResult result;
				std::memcpy(&result, message.data(), sizeof(result));
				status = static_cast<plugin::ProcessStatus>(result.status);
			}
		}
		audio.releaseBlock();
		return status;
	}

	// A pair of control connections in this process, with small rings.
	struct ControlPair
	{
		ControlPair()
		{
			ipc::Config config = plugin::remote::controlConfig();
			config.controlRingSize = 4096;
			engine = ipc::Connection::create(config);
			host = ipc::Connection::open(engine.name());
		}

		ipc::Connection engine;
		ipc::Connection host;
	};

	// Reads until a complete message arrives or the timeout passes.
	ipc::ReadStatus readMessage(plugin::remote::MessageReader& reader, ipc::Connection& connection,
		plugin::remote::ControlMessage& message, std::chrono::milliseconds timeout = 5000ms)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		for (;;)
		{
			const ipc::ReadStatus status = reader.read(message);
			if (status != ipc::ReadStatus::Empty || std::chrono::steady_clock::now() >= deadline)
				return status;
			connection.waitForControl(10ms);
		}
	}

	std::vector<uint8_t> pattern(size_t size, uint8_t seed)
	{
		std::vector<uint8_t> bytes(size);
		for (size_t i = 0; i < size; ++i)
			bytes[i] = static_cast<uint8_t>((i * 31) + seed);
		return bytes;
	}
}

TEST_CASE("a test plugin processes audio through the host process sample-exactly", "[integration]")
{
	plugins::HostManager manager(testOptions());
	LocalHost host;
	SlotGraph inProcess(localPlugin("test:gain", host));
	SlotGraph hosted(createRemote(manager));

	Noise noise;
	const uint32_t sizes[] = {256, 1, 100, 255, 64, 17, 256};
	bool changed = false;
	for (int block = 0; block < 60; ++block)
	{
		const uint32_t frames = sizes[static_cast<size_t>(block) % std::size(sizes)];
		const std::vector<float> input = noise.block(frames);
		// Gain changes from -30 to +12 dB at random frames.
		std::vector<graph::ProcessEvent> events;
		events.reserve(4);
		for (uint32_t i = 0; i < noise.next() % 4; ++i)
			events.push_back(parameter(noise.next() % frames, g_gain, -30.0 + static_cast<double>(noise.next() % 43)));
		std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
		changed = changed || !events.empty();

		const std::vector<float> expected = inProcess.render(input, events);
		const std::vector<float> actual = hosted.render(input, events);
		REQUIRE(actual == expected);
		REQUIRE(!hosted.slot().failed());
	}
	CHECK(changed);

	// The engine mirrors the parameter values the plugin got.
	CHECK(valueOf(hosted.slot().plugin(), g_gain) == valueOf(inProcess.slot().plugin(), g_gain));
}

TEST_CASE("the host process answers control calls and exits when the engine closes it", "[integration]")
{
	plugins::HostManager manager(testOptions());
	uint32_t process = 0;
	{
		const std::unique_ptr<plugin::remote::RemotePlugin> remote = createRemote(manager);
		process = remote->host()->processId();
		CHECK(remote->descriptor().name == "GNUbase Test Gain");
		CHECK(remote->descriptor().vendor == "GNUbase");
		CHECK(remote->audioPortCount(true) == 1);
		CHECK(remote->paramCount() == 1);
		CHECK(remote->extension("gnubase.gui") == nullptr);
		CHECK(remote->setProcessMode(plugin::ProcessMode::Offline));

		std::string text;
		REQUIRE(remote->paramValueToText(g_gain, -6.0, text));
		CHECK(text == "-6.000 dB");
		double value = 0.0;
		REQUIRE(remote->paramTextToValue(g_gain, "-3", value));
		CHECK(value == -3.0);

		// Changes while the plugin doesn't process go through flushParams on the main thread.
		flush(*remote, {paramEvent(g_gain, -12.0)});
		CHECK(valueOf(*remote, g_gain) == -12.0);

		// State saved in the host loads into another instance.
		const std::vector<uint8_t> state = stateOf(*remote);
		const std::unique_ptr<plugin::remote::RemotePlugin> other = createRemote(manager);
		REQUIRE(other->loadState(state.data(), state.size(), plugin::StateContext::Project));
		CHECK(valueOf(*other, g_gain) == -12.0);
		CHECK(stateOf(*other) == state);
		CHECK(manager.hosts().size() == 1);

		// The process mode only changes while inactive.
		REQUIRE(other->activate(g_sampleRate, 1, g_maxFrames));
		CHECK(!other->setProcessMode(plugin::ProcessMode::Realtime));
		other->deactivate();
	}
	// The last plugin of the process is gone, so the manager closed the connection and the process exited.
	CHECK(manager.hosts().empty());
	CHECK(eventually([&] { return !platform::processExists(process); }));
}

TEST_CASE("plugins load as jobs, off the calling thread", "[integration]")
{
	plugins::HostManager manager(testOptions());
	std::future<std::unique_ptr<plugin::remote::RemotePlugin>> loading = manager.load(gain());
	const std::unique_ptr<plugin::remote::RemotePlugin> remote = loading.get();
	REQUIRE(remote);
	CHECK(remote->descriptor().id == "test:gain");
	CHECK(remote->paramCount() == 1);

	// Slow plugin calls run as jobs too, one after the other.
	std::vector<uint8_t> state;
	bool saved = false;
	manager.run([&] { saved = remote->saveState(state, plugin::StateContext::Project); }).get();
	CHECK(saved);
	CHECK(!state.empty());
	CHECK_THROWS(manager.load({"test:nothing", "", "GNUbase"}).get());
	CHECK_THROWS_AS(manager.run([] { throw std::runtime_error("failed"); }).get(), std::runtime_error);
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

TEST_CASE("the host process checks blocks and activation against what the plugin was activated with", "[integration]")
{
	plugin::remote::LaunchOptions options;
	options.executable = GNUBASE_PLUGINHOST;
	const std::shared_ptr<plugin::remote::HostConnection> host = plugin::remote::HostConnection::launch(options);
	const uint32_t instance = host->newInstance();
	plugin::remote::Writer create;
	create.string("test:gain");
	create.string("");
	REQUIRE(host->request(plugin::remote::Message::Create, instance, create.data(), 5s).ok);

	ipc::Connection audio = ipc::Connection::create(plugin::remote::audioConfig(2, 2, g_maxFrames));
	const auto activate = [&](double sampleRate, uint32_t minFrames, uint32_t maxFrames)
	{
		plugin::remote::Writer writer;
		writer.f64(sampleRate);
		writer.u32(minFrames);
		writer.u32(maxFrames);
		writer.string(audio.name());
		return host->request(plugin::remote::Message::Activate, instance, writer.data(), 5s).ok;
	};
	CHECK(!activate(0.0, 1, g_maxFrames));
	CHECK(!activate(std::numeric_limits<double>::quiet_NaN(), 1, g_maxFrames));
	CHECK(!activate(-48000.0, 1, g_maxFrames));
	CHECK(!activate(g_sampleRate, 0, g_maxFrames));
	CHECK(!activate(g_sampleRate, 300, g_maxFrames));
	CHECK(!activate(g_sampleRate, 1, g_maxFrames * 2)); // larger than the audio connection, which is now closed
	audio = ipc::Connection::create(plugin::remote::audioConfig(2, 2, g_maxFrames));
	// Smaller blocks than the connection has room for, so the connection takes the blocks that are too large.
	constexpr uint32_t maxFrames = 128;
	REQUIRE(activate(g_sampleRate, 1, maxFrames));

	const auto flushRequest = [&]
	{
		plugin::EventList events(2);
		events.push(paramEvent(g_gain, -6.0));
		plugin::remote::Writer writer;
		plugin::remote::write(writer, events);
		return host->request(plugin::remote::Message::FlushParams, instance, writer.data(), 5s).ok;
	};

	// Blocks only come after a Start, which takes effect at once.
	uint64_t sequence = 0;
	CHECK(exchangeRaw(audio, ++sequence, {64, 0, 1, {0}, {}}) == plugin::ProcessStatus::Error);
	CHECK(exchangeRaw(audio, ++sequence, {0, plugin::remote::Start, 0, {}, {}}) == plugin::ProcessStatus::Continue);
	// While the instance processes, flushes go with the blocks, not over the control connection.
	CHECK(!flushRequest());
	CHECK(exchangeRaw(audio, ++sequence, {64, 0, 1, {0}, {}}) == plugin::ProcessStatus::Continue);

	// Invalid blocks fail without reaching the plugin.
	CHECK(exchangeRaw(audio, ++sequence, {maxFrames + 1, 0, 1, {0}, {}}) == plugin::ProcessStatus::Error);
	plugin::Event late = paramEvent(g_gain, -6.0);
	late.offset = 64;
	CHECK(exchangeRaw(audio, ++sequence, {64, 0, 1, {0}, {late}}) == plugin::ProcessStatus::Error);
	CHECK(exchangeRaw(audio, ++sequence, {64, 0, 2, {0}, {}}) == plugin::ProcessStatus::Error); // a segment missing
	CHECK(exchangeRaw(audio, ++sequence, {64, 0, 1, {5}, {}}) == plugin::ProcessStatus::Error); // not at frame 0
	CHECK(exchangeRaw(audio, ++sequence, {64, 0, 2, {0, 64}, {}}) == plugin::ProcessStatus::Error);
	CHECK(exchangeRaw(audio, ++sequence, {64, 0, 0, {}, {}}) == plugin::ProcessStatus::Error); // no transport
	CHECK(exchangeRaw(audio, ++sequence, {64, 0, 1, {0}, {}}) == plugin::ProcessStatus::Continue);

	// After a Stop, the control connection takes flushes again.
	CHECK(exchangeRaw(audio, ++sequence, {0, plugin::remote::Stop, 0, {}, {}}) == plugin::ProcessStatus::Continue);
	CHECK(flushRequest());
	CHECK(!host->exited());
}

TEST_CASE("starting and stopping a remote plugin takes effect at once", "[integration]")
{
	plugins::HostManager manager(testOptions());
	const std::unique_ptr<plugin::remote::RemotePlugin> remote = createRemote(manager);
	LocalHost host;
	const std::unique_ptr<plugin::Plugin> local = localPlugin("test:gain", host);
	REQUIRE(remote->activate(g_sampleRate, 1, g_maxFrames));
	REQUIRE(remote->startProcessing());

	// While it processes, a flush goes to the host with a block of its own.
	flush(*remote, {paramEvent(g_gain, -6.0)});
	flush(*local, {paramEvent(g_gain, -6.0)});
	CHECK(stateOf(*remote) == stateOf(*local));

	// Stopped: the host takes flushes on its main thread again, which it refuses while the plugin processes.
	remote->stopProcessing();
	flush(*remote, {paramEvent(g_gain, -12.0)});
	flush(*local, {paramEvent(g_gain, -12.0)});
	CHECK(stateOf(*remote) == stateOf(*local));
	CHECK(remote->failure() == plugin::remote::Failure::None);
	remote->deactivate();
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
		graphs[i]->render(input, {parameter(0, g_gain, gains[i])});
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
			CHECK(valueOf(graphs[i]->slot().plugin(), g_gain) == gains[i]);
		}
	};

	CHECK(manager.setMode(plugins::HostingMode::PerInstance).get().empty());
	check(3);
	CHECK(manager.setMode(plugins::HostingMode::Shared).get().empty());
	check(1);
	// A plugin with an override of its own.
	CHECK(manager.setOverride("test:gain", plugins::HostingMode::PerInstance).get().empty());
	check(3);
	CHECK(manager.setOverride("test:gain", std::nullopt).get().empty());
	check(1);
	CHECK(manager.setMode(plugins::HostingMode::PerPlugin).get().empty());
	check(1);
	CHECK(manager.setMode(plugins::HostingMode::PerVendor).get().empty());
	check(2);
}

TEST_CASE("a plugin with a state larger than the control ring keeps it when it moves", "[integration]")
{
	plugins::HostManager manager(testOptions());
	std::unique_ptr<plugin::remote::RemotePlugin> remote = createRemote(manager, {"test:stateblob", "", "GNUbase"});
	// 4 MiB of state: more than the control connection's ring holds, so it goes in parts.
	flush(*remote, {paramEvent(g_seed, 777.0), paramEvent(g_stateSize, 4.0 * 1024 * 1024)});
	SlotGraph graph(std::move(remote));
	Noise noise;
	graph.render(noise.block(64));
	const std::vector<uint8_t> state = stateOf(graph.slot().plugin());
	CHECK(state.size() > plugin::remote::controlConfig().controlRingSize * size_t{2});
	const uint32_t before = manager.hosts().at(0).process;

	CHECK(manager.setMode(plugins::HostingMode::PerInstance).get().empty());
	CHECK(manager.hosts().at(0).process != before);
	CHECK(stateOf(graph.slot().plugin()) == state);
	CHECK(valueOf(graph.slot().plugin(), g_seed) == 777.0);
	graph.render(noise.block(64));
	CHECK(!graph.slot().failed());
}

TEST_CASE("a plugin moves to another host process while it processes, without a gap", "[integration]")
{
	plugins::HostManager manager(testOptions());
	SlotGraph graph(createRemote(manager));
	Noise noise;
	const std::vector<float> input = noise.block(64);
	graph.render(input, {parameter(0, g_gain, -6.0)});
	const std::vector<float> expected = graph.render(input);

	// The audio thread renders on while the job thread moves the plugin back and forth.
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
		CHECK(manager.setMode(i % 2 == 0 ? plugins::HostingMode::PerInstance : plugins::HostingMode::Shared)
				.get()
				.empty());
		processes.push_back(manager.hosts().at(0).process);
	}
	const int before = blocks;
	CHECK(eventually([&] { return blocks > before + 10; }));
	stop = true;
	audio.join();
	CHECK(wrong == 0);
	CHECK(processes[0] != processes[1]); // it did move
}

TEST_CASE("parameter changes while a plugin moves reach its new instance", "[integration]")
{
	plugins::HostManager manager(testOptions());
	SlotGraph graph(createRemote(manager));
	Noise noise;
	const std::vector<float> input = noise.block(64);
	graph.render(input);

	// A new gain with every block while the plugin moves: most go to the old instance after its state was saved.
	std::future<std::vector<std::string>> moved = manager.setMode(plugins::HostingMode::PerInstance);
	double last = 0.0;
	int changes = 0;
	while (moved.wait_for(0s) != std::future_status::ready)
	{
		last = -40.0 + static_cast<double>(changes++ % 50);
		graph.render(input, {parameter(0, g_gain, last)});
	}
	CHECK(moved.get().empty());
	CHECK(changes > 0);

	// No more changes: the new instance has the last one.
	LocalHost host;
	SlotGraph reference(localPlugin("test:gain", host));
	reference.render(input, {parameter(0, g_gain, last)});
	CHECK(graph.render(input) == reference.render(input));
	CHECK(valueOf(graph.slot().plugin(), g_gain) == last);
	CHECK(!graph.slot().failed());
}

TEST_CASE("a remote plugin's requests and changes reach the engine", "[integration]")
{
	plugins::HostManager manager(testOptions());
	EngineHost engineHost;
	std::unique_ptr<plugin::remote::RemotePlugin> remote =
		manager.load({"test:latency", "", "GNUbase"}, &engineHost).get();
	plugin::remote::RemotePlugin& plugin = *remote;
	SlotGraph graph(std::move(remote));
	Noise noise;

	// Activated, the plugin reports its latency.
	CHECK(eventually([&] { return engineHost.latencyChanges == 1; }));
	CHECK(plugin.latency() == 64);

	// A new latency while it processes: it asks for a restart, and reports the latency when activated again.
	graph.render(noise.block(64), {parameter(0, g_latency, 128.0)});
	CHECK(eventually([&] { return engineHost.restarts == 1; }));
	graph.slot().prepareFor(g_sampleRate, g_maxFrames / 2); // the restart: activated again
	CHECK(eventually([&] { return engineHost.latencyChanges == 2; }));
	CHECK(plugin.latency() == 128);
	CHECK(graph.slot().latency() == 128);

	// A loaded state changes the values: the engine reads them again.
	const std::vector<uint8_t> state = stateOf(plugin);
	bool loaded = false;
	manager.run([&] { loaded = plugin.loadState(state.data(), state.size(), plugin::StateContext::Project); }).get();
	CHECK(loaded);
	CHECK(eventually([&] { return engineHost.valueRescans == 1; }));
	CHECK(!graph.slot().failed());
}

TEST_CASE("an active plugin keeps its latency when it moves to another host process", "[integration]")
{
	// test:latency reports its latency only while active: the info its new instance has before activation says 0.
	plugins::HostManager manager(testOptions());
	std::unique_ptr<plugin::remote::RemotePlugin> remote = createRemote(manager, {"test:latency", "", "GNUbase"});
	const plugin::remote::RemotePlugin& plugin = *remote;
	SlotGraph graph(std::move(remote));
	Noise noise;
	graph.render(noise.block(64));
	REQUIRE(plugin.latency() == 64);
	const uint32_t before = manager.hosts().at(0).process;

	CHECK(manager.setMode(plugins::HostingMode::PerInstance).get().empty());
	CHECK(manager.hosts().at(0).process != before); // it did move
	CHECK(plugin.latency() == 64);
	CHECK(graph.slot().latency() == 64);
	graph.render(noise.block(64));
	CHECK(!graph.slot().failed());
}

TEST_CASE("killing the host process mid-render yields silence and an error event", "[integration]")
{
	Engine engine;
	Client client(engine);
	plugins::HostManager& manager = engine.pluginHosts();
	manager.setExecutable(GNUBASE_PLUGINHOST);
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

TEST_CASE("a block's deadline follows the block period", "[unit]")
{
	const plugin::remote::Deadlines deadlines;
	CHECK(deadlines.forBlock(128, 48000.0, false) == std::chrono::nanoseconds(5333333));
	CHECK(deadlines.forBlock(16, 48000.0, false) == 2ms);      // at least the minimum
	CHECK(deadlines.forBlock(48000, 48000.0, false) == 100ms); // at most the maximum
	CHECK(deadlines.forBlock(128, 48000.0, true) == 10s);      // offline
	CHECK(deadlines.forBlock(128, 0.0, false) == deadlines.realtimeMaximum);
}

TEST_CASE("a late block is silence for that plugin and that block only", "[integration]")
{
	plugins::HostManager::Options options = testOptions();
	options.deadlines.realtimeMinimum = 200ms;
	options.deadlines.realtimeMaximum = 200ms;
	options.deadlines.hang = 10s; // so a slow machine can't make the test plugin hang
	plugins::HostManager manager(options);
	std::unique_ptr<plugin::remote::RemotePlugin> remote = createRemote(manager, {"test:stall", "", "GNUbase"});
	const plugin::remote::RemotePlugin& plugin = *remote;
	SlotGraph stalling(std::move(remote), plugin::ProcessMode::Realtime);
	SlotGraph other(createRemote(manager), plugin::ProcessMode::Realtime);
	REQUIRE(manager.hosts().size() == 1); // the same vendor: the same process
	LocalHost host;
	SlotGraph reference(localPlugin("test:stall", host), plugin::ProcessMode::Realtime);
	SlotGraph otherReference(localPlugin("test:gain", host), plugin::ProcessMode::Realtime);
	Noise noise;
	std::vector<std::vector<float>> inputs;
	inputs.reserve(20);
	for (int i = 0; i < 20; ++i)
		inputs.push_back(noise.block(128));
	REQUIRE(stalling.render(inputs[0]) == reference.render(inputs[0]));

	// The block in which the plugin takes 300 ms misses its 200 ms deadline: it is silent.
	CHECK(silent(stalling.render(inputs[1], {parameter(0, 1, 300.0)})));
	CHECK(stalling.slot().failed());
	CHECK(plugin.failure() == plugin::remote::Failure::None);
	// Meanwhile the other plugin of its host process answers in time.
	CHECK(other.render(inputs[1]) == otherReference.render(inputs[1]));
	CHECK(!other.slot().failed());

	// Then the plugin goes on: each block gets its own answer, never the late one, and its host isn't hung.
	int late = 0;
	for (size_t i = 2; i < inputs.size(); ++i)
	{
		const std::vector<float> output = stalling.render(inputs[i]);
		if (silent(output))
		{
			++late; // still catching up
			continue;
		}
		CHECK(output == reference.render(inputs[i]));
		CHECK(!stalling.slot().failed());
	}
	CHECK(late <= 1);
	CHECK(plugin.failure() == plugin::remote::Failure::None);
	CHECK(other.render(inputs[0]) == otherReference.render(inputs[0]));
	CHECK(manager.hosts().at(0).state == "running");
	CHECK(manager.takeFailures().empty());
}

TEST_CASE("a plugin that answers no block for the hang timeout fails, and so do the other plugins of its host",
	"[integration]")
{
	plugins::HostManager::Options options = testOptions();
	options.deadlines.realtimeMinimum = 20ms;
	options.deadlines.realtimeMaximum = 20ms;
	options.deadlines.hang = 300ms;
	plugins::HostManager manager(options);
	std::unique_ptr<plugin::remote::RemotePlugin> remote = createRemote(manager, {"test:hang", "", "GNUbase"});
	const plugin::remote::RemotePlugin& plugin = *remote;
	SlotGraph hanging(std::move(remote), plugin::ProcessMode::Realtime);
	SlotGraph other(createRemote(manager, {"test:hang", "", "GNUbase"}), plugin::ProcessMode::Realtime);
	REQUIRE(manager.hosts().size() == 1); // the same vendor: the same process
	Noise noise;
	const std::vector<float> input = noise.block(128);
	CHECK(!silent(other.render(input)));

	// Its blocks are silent, but until the hang timeout it hasn't failed, and the other plugin goes on.
	const auto start = std::chrono::steady_clock::now();
	CHECK(silent(hanging.render(input, {parameter(0, g_hang, 1.0)})));
	CHECK(plugin.failure() == plugin::remote::Failure::None);
	CHECK(!silent(other.render(input)));
	CHECK(!other.slot().failed());
	while (plugin.failure() == plugin::remote::Failure::None && std::chrono::steady_clock::now() - start < 5s)
	{
		CHECK(silent(hanging.render(input)));
		std::this_thread::sleep_for(1ms);
	}
	const auto elapsed = std::chrono::steady_clock::now() - start;
	CHECK(plugin.failure() == plugin::remote::Failure::Deadline);
	CHECK(elapsed >= 300ms);
	CHECK(elapsed < 1300ms);

	// Then its host is hung: later blocks don't wait for it, nor for the other plugins of its process.
	auto next = std::chrono::steady_clock::now();
	CHECK(silent(hanging.render(input)));
	CHECK(std::chrono::steady_clock::now() - next < 100ms);
	next = std::chrono::steady_clock::now();
	CHECK(silent(other.render(input)));
	CHECK(std::chrono::steady_clock::now() - next < 100ms);
	CHECK(other.slot().failed());

	std::vector<plugins::PluginFailure> failures;
	CHECK(eventually(
		[&]
		{
			std::vector<plugins::PluginFailure> taken = manager.takeFailures();
			failures.insert(failures.end(), taken.begin(), taken.end());
			return failures.size() == 2;
		}));
	REQUIRE(failures.size() == 2);
	CHECK(failures[0].plugin == "test:hang");
	CHECK(failures[0].reason == "hung");
	CHECK(failures[1].reason == "hung");
	CHECK(manager.hosts().at(0).state == "hung");
}

TEST_CASE("an offline block that misses its deadline makes the plugin hang at once", "[integration]")
{
	plugins::HostManager::Options options = testOptions();
	options.deadlines.offline = 300ms; // longer than the hang timeout
	options.deadlines.hang = 250ms;
	plugins::HostManager manager(options);
	std::unique_ptr<plugin::remote::RemotePlugin> remote = createRemote(manager, {"test:hang", "", "GNUbase"});
	const plugin::remote::RemotePlugin& plugin = *remote;
	SlotGraph hanging(std::move(remote), plugin::ProcessMode::Offline);
	Noise noise;
	const std::vector<float> input = noise.block(128);
	CHECK(silent(hanging.render(input, {parameter(0, g_hang, 1.0)})));
	CHECK(plugin.failure() == plugin::remote::Failure::Deadline);
	CHECK(manager.hosts().at(0).state == "hung");
}

TEST_CASE("a host process exits when the engine dies, even while a plugin blocks its main thread", "[integration]")
{
	const std::filesystem::path file = std::filesystem::temp_directory_path() /
		("gnubase-hung-host-" + std::to_string(platform::currentProcessId()) + ".txt");
	std::filesystem::remove(file);
	platform::ChildProcess engine =
		platform::ChildProcess::start(GNUBASE_PLUGINHOST, {"--play-engine", file.u8string()});
	REQUIRE(eventually([&] { return std::filesystem::exists(file); }, 20000ms));
	uint32_t host = 0;
	std::ifstream(file) >> host;
	std::filesystem::remove(file);
	REQUIRE(host != 0);
	CHECK(platform::processExists(host));

	engine.kill();
	CHECK(eventually([&] { return !platform::processExists(host); }));
}

TEST_CASE("the hosting mode is set through engine commands, and the moves are reported", "[integration]")
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

	// The moves run in the background; when they are done, the engine says so.
	bool changed = false;
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while (!changed && std::chrono::steady_clock::now() < deadline)
	{
		const std::optional<Message> message = client.wait(100ms);
		const auto* event = message ? std::get_if<Event>(&*message) : nullptr;
		if (event != nullptr && event->name == "pluginhost.changed")
		{
			CHECK(event->data.at("mode") == "perInstance");
			CHECK(!event->data.contains("errors"));
			changed = true;
		}
	}
	CHECK(changed);

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

TEST_CASE("control messages larger than the ring arrive in parts", "[unit]")
{
	ControlPair pair;
	plugin::remote::MessageWriter writer(pair.engine);
	plugin::remote::MessageReader reader(pair.host);
	const std::vector<uint8_t> body = pattern(20000, 7); // five times the ring
	plugin::remote::SendResult sent = plugin::remote::SendResult::Closed;
	std::thread sender([&] { sent = writer.send(plugin::remote::Message::LoadState, {3, 4}, body, 5s); });
	plugin::remote::ControlMessage message;
	const ipc::ReadStatus status = readMessage(reader, pair.host, message);
	sender.join();
	CHECK(sent == plugin::remote::SendResult::Sent);
	REQUIRE(status == ipc::ReadStatus::Ok);
	CHECK(message.type == plugin::remote::Message::LoadState);
	CHECK(message.header.request == 3);
	CHECK(message.header.instance == 4);
	CHECK(message.body == body);
	CHECK(!message.tooLarge);
	CHECK(reader.droppedParts() == 0);
}

TEST_CASE("a control message whose sender gave up halfway never puts the stream out of step", "[unit]")
{
	ControlPair pair;
	plugin::remote::MessageWriter writer(pair.engine);
	plugin::remote::MessageReader reader(pair.host);

	// Nobody reads: the sender gives up after the parts that fit.
	CHECK(writer.send(plugin::remote::Message::LoadState, {1, 1}, pattern(20000, 1), 50ms) ==
		plugin::remote::SendResult::Timeout);
	plugin::remote::ControlMessage message;
	CHECK(reader.read(message) == ipc::ReadStatus::Empty); // the partial message never completes

	// The next message arrives whole, and the parts of the partial one are dropped.
	const std::vector<uint8_t> body = pattern(9000, 2);
	plugin::remote::SendResult sent = plugin::remote::SendResult::Closed;
	std::thread sender([&] { sent = writer.send(plugin::remote::Message::SaveState, {2, 1}, body, 5s); });
	const ipc::ReadStatus status = readMessage(reader, pair.host, message);
	sender.join();
	CHECK(sent == plugin::remote::SendResult::Sent);
	REQUIRE(status == ipc::ReadStatus::Ok);
	CHECK(message.type == plugin::remote::Message::SaveState);
	CHECK(message.header.request == 2);
	CHECK(message.body == body);
	CHECK(reader.droppedParts() > 0);

	// A part without its header breaks the protocol.
	const uint32_t garbage = 0;
	REQUIRE(pair.engine.sendControl(static_cast<uint16_t>(plugin::remote::Message::Reply), 1, &garbage, 2));
	CHECK(readMessage(reader, pair.host, message, 1000ms) == ipc::ReadStatus::Corrupt);
}

TEST_CASE("control messages over the size limit are refused before they are sent", "[unit]")
{
	ControlPair pair;
	plugin::remote::MessageWriter small(pair.engine, 1000);
	plugin::remote::MessageReader reader(pair.host);
	CHECK(small.send(plugin::remote::Message::LoadState, {1, 1}, pattern(2000, 3), 1s) ==
		plugin::remote::SendResult::TooLarge);
	plugin::remote::ControlMessage message;
	CHECK(reader.read(message) == ipc::ReadStatus::Empty); // nothing went out

	// A receiver with a smaller limit gets the header only, so it can answer with an error.
	plugin::remote::MessageWriter writer(pair.engine);
	plugin::remote::MessageReader limited(pair.host, 1000);
	plugin::remote::SendResult sent = plugin::remote::SendResult::Closed;
	std::thread sender([&] { sent = writer.send(plugin::remote::Message::LoadState, {5, 6}, pattern(3000, 4), 5s); });
	const ipc::ReadStatus status = readMessage(limited, pair.host, message);
	sender.join();
	CHECK(sent == plugin::remote::SendResult::Sent);
	REQUIRE(status == ipc::ReadStatus::Ok);
	CHECK(message.tooLarge);
	CHECK(message.header.request == 5);
	CHECK(message.header.instance == 6);
	CHECK(message.body.empty());

	// The next message is fine.
	std::thread next([&] { sent = writer.send(plugin::remote::Message::Describe, {7, 6}, {}, 5s); });
	CHECK(readMessage(limited, pair.host, message) == ipc::ReadStatus::Ok);
	next.join();
	CHECK(!message.tooLarge);
	CHECK(message.header.request == 7);
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

TEST_CASE("plugin infos with empty names round-trip, however many parameters they have", "[unit]")
{
	// Unnamed, one-letter and numbered parameters and ports are common; empty names make each record its shortest.
	plugin::remote::PluginInfo info;
	info.descriptor.id = "test:gain";
	info.extensions = {"params"};
	for (const uint32_t id : {0U, 1U})
	{
		plugin::AudioPortInfo port;
		port.id = id;
		port.channelCount = 2;
		info.inputs.push_back(port);
		info.outputs.push_back(port);
	}
	for (uint32_t i = 0; i < 100; ++i)
	{
		plugin::ParamInfo param;
		param.id = i + 1;
		param.maximum = 1.0;
		param.name = i % 2 == 0 ? "" : "A";
		info.params.push_back(param);
		info.values.push_back(static_cast<double>(i) / 100.0);
	}
	info.latency = 64;
	info.tail = 7;
	plugin::remote::Writer writer;
	plugin::remote::write(writer, info);

	plugin::remote::Reader reader(writer.data());
	plugin::remote::PluginInfo read;
	REQUIRE(plugin::remote::read(reader, read));
	CHECK(reader.remaining() == 0);
	CHECK(read.inputs.size() == 2);
	CHECK(read.outputs.size() == 2);
	REQUIRE(read.params.size() == 100);
	CHECK(read.params[0].name.empty());
	CHECK(read.params[0].module.empty());
	CHECK(read.params[99].id == 100);
	CHECK(read.params[99].name == "A");
	CHECK(read.values == info.values);
	CHECK(read.latency == 64);
	CHECK(read.tail == 7);

	// One byte short is still refused.
	plugin::remote::Reader truncated(writer.data().data(), writer.data().size() - 1);
	plugin::remote::PluginInfo partial;
	CHECK(!plugin::remote::read(truncated, partial));
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
