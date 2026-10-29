// SPDX-License-Identifier: GPL-3.0-or-later
// Parallel processing (#212): the graph's nodes run on a worker pool, and the output is bit-identical for any
// number of threads.
#include "engine/engine.h"
#include "engine/graph/graph.h"
#include "platform/rt/workerpool.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace gnubase::engine;
using namespace gnubase::engine::graph;
using Catch::Matchers::ContainsSubstring;

namespace
{
	constexpr double g_sampleRate = 48000.0;
	constexpr uint32_t g_maxFrames = 128;

	// Adds a value, a running state, the notes it got and a gain from parameter 0 to its input. Float sums in a
	// different order give different bits, so any change in the order of a node's inputs shows in the output.
	class Busy : public Processor
	{
	public:
		Busy(float value, int work) : m_value(value), m_work(work) {}

		void process(const ProcessContext& context) noexcept override
		{
			size_t next = 0;
			for (uint32_t i = 0; i < context.frames; ++i)
			{
				for (; next < context.eventsIn.size() && context.eventsIn[next].offset <= i; ++next)
				{
					const ProcessEvent& event = context.eventsIn[next];
					if (event.type == ProcessEvent::Type::Parameter)
						m_gain = static_cast<float>(event.parameterValue);
					else if (event.type == ProcessEvent::Type::NoteOn)
						m_state += static_cast<float>(event.key);
					context.eventsOut.add(event); // and passes it on
				}
				m_state = (m_state * 0.999f) + m_value;
				for (int c = 0; c < context.channels; ++c)
					context.audio[c][i] = (context.audio[c][i] * m_gain) + m_state + static_cast<float>(c);
			}
			volatile float waste = 0.0f; // some nodes take longer, so threads interleave differently
			for (int i = 0; i < m_work; ++i)
				waste = waste + 1.0f;
		}

	private:
		float m_value;
		int m_work;
		float m_state = 0.0f;
		float m_gain = 1.0f;
	};

	// A random graph: nodes connect to later nodes only, so it has no cycle. Built anew for every run, since the
	// processors keep state.
	GraphDesc randomGraph(uint32_t seed)
	{
		std::mt19937 random(seed);
		const auto pick = [&random](int from, int to) { return std::uniform_int_distribution<int>(from, to)(random); };
		const float values[] = {1e8f, -1e8f, 1.0f, 0.25f, 3e7f, -0.5f};
		GraphDesc graph;
		const int size = pick(5, 60);
		for (int i = 0; i < size; ++i)
		{
			NodeDesc node;
			node.kind = i == size - 1 ? NodeKind::OutputBus : NodeKind::Channel;
			node.channels = pick(0, 4) == 0 ? 1 : 2;
			if (pick(0, 5) != 0)
				node.processor = std::make_shared<Busy>(values[pick(0, 5)], pick(0, 3) == 0 ? 20000 : 0);
			graph.nodes.push_back(node);
			for (int k = pick(0, 4); i > 0 && k > 0; --k)
				graph.audio.push_back({pick(0, i - 1), i});
			if (i > 0 && pick(0, 3) == 0)
				graph.events.push_back({pick(0, i - 1), i});
		}
		return graph;
	}

	// Every node's audio, block after block, with notes and parameter changes on the way.
	std::vector<float> render(uint32_t seed, gnubase::rt::WorkerPool* pool)
	{
		const GraphDesc desc = randomGraph(seed);
		const CompiledGraph graph(desc, g_sampleRate, g_maxFrames);
		std::mt19937 random(seed + 1);
		const auto pick = [&random](int from, int to) { return std::uniform_int_distribution<int>(from, to)(random); };
		std::vector<float> result;
		for (int block = 0; block < 20; ++block)
		{
			const auto frames = static_cast<uint32_t>(pick(1, static_cast<int>(g_maxFrames)));
			for (int e = pick(0, 6); e > 0; --e)
			{
				ProcessEvent event;
				event.offset = static_cast<uint32_t>(pick(0, static_cast<int>(frames) - 1));
				event.type = pick(0, 1) == 0 ? ProcessEvent::Type::NoteOn : ProcessEvent::Type::Parameter;
				event.key = static_cast<uint8_t>(pick(0, 127));
				event.parameterValue = pick(0, 4) * 0.25;
				graph.events(pick(0, static_cast<int>(graph.size()) - 1)).add(event);
			}
			graph.process(frames, TransportBlock{}, pool);
			for (int node = 0; node < static_cast<int>(graph.size()); ++node)
				for (int c = 0; c < desc.nodes[static_cast<size_t>(node)].channels; ++c)
					result.insert(result.end(), graph.audio(node)[c], graph.audio(node)[c] + frames);
		}
		return result;
	}
}

TEST_CASE("random graphs render bit-identically with any number of threads", "[unit]")
{
	std::vector<std::unique_ptr<gnubase::rt::WorkerPool>> pools;
	for (const size_t workers : {1, 2, 3, 5, 7})
		pools.push_back(std::make_unique<gnubase::rt::WorkerPool>(workers));
	for (uint32_t seed = 1; seed <= 40; ++seed)
	{
		CAPTURE(seed);
		const std::vector<float> serial = render(seed, nullptr);
		for (const auto& pool : pools)
		{
			CAPTURE(pool->workers());
			const std::vector<float> parallel = render(seed, pool.get());
			REQUIRE(parallel.size() == serial.size());
			CHECK(std::memcmp(parallel.data(), serial.data(), serial.size() * sizeof(float)) == 0);
		}
	}
}

TEST_CASE("a graph runs repeatedly on a pool without its threads getting in each other's way", "[unit]")
{
	// One wide layer of busy nodes feeding one bus: many tasks become ready at once and finish together.
	GraphDesc desc;
	for (int i = 0; i < 32; ++i)
		desc.nodes.push_back({NodeKind::Channel, 0, 2, std::make_shared<Busy>(static_cast<float>(i), 2000)});
	desc.nodes.push_back({NodeKind::OutputBus, 0, 2, nullptr});
	for (int i = 0; i < 32; ++i)
		desc.audio.push_back({i, 32});
	gnubase::rt::WorkerPool pool(4);
	const CompiledGraph parallel(desc, g_sampleRate, g_maxFrames);
	for (int block = 0; block < 300; ++block)
		parallel.process(g_maxFrames, TransportBlock{}, &pool);
	std::vector<float> out(parallel.audio(32)[0], parallel.audio(32)[0] + g_maxFrames);

	for (int i = 0; i < 32; ++i)
		desc.nodes[static_cast<size_t>(i)].processor = std::make_shared<Busy>(static_cast<float>(i), 0);
	const CompiledGraph serial(desc, g_sampleRate, g_maxFrames);
	for (int block = 0; block < 300; ++block)
		serial.process(g_maxFrames);
	CHECK(out == std::vector<float>(serial.audio(32)[0], serial.audio(32)[0] + g_maxFrames));
}

namespace
{
	// A tail that decays through the subnormal range, like a fade, a filter or a reverb: each sample is a quarter of
	// the last plus a tiny value of its own.
	class Tail : public Processor
	{
	public:
		explicit Tail(float start) : m_start(start), m_state(start) {}

		void process(const ProcessContext& context) noexcept override
		{
			for (uint32_t i = 0; i < context.frames; ++i)
			{
				const volatile float tiny = 3e-39f;
				m_state = (m_state * 0.25f) + tiny;
				if (m_state == 0.0f)
					m_state = m_start; // again, so every block has a tail
				for (int c = 0; c < context.channels; ++c)
					context.audio[c][i] = (context.audio[c][i] * 0.5f) + m_state;
			}
		}

	private:
		float m_start;
		float m_state;
	};

	// Many tails into a few busses into one: busses sum values around 1e-39.
	std::vector<float> renderTails(gnubase::rt::WorkerPool* pool)
	{
		GraphDesc desc;
		for (int i = 0; i < 24; ++i)
			desc.nodes.push_back({NodeKind::Channel, 0, 2, std::make_shared<Tail>(1e-36f * static_cast<float>(i + 1))});
		for (int i = 0; i < 4; ++i)
			desc.nodes.push_back({NodeKind::Channel, 0, 2, std::make_shared<Tail>(2e-38f)});
		desc.nodes.push_back({NodeKind::OutputBus, 0, 2, nullptr});
		for (int i = 0; i < 24; ++i)
			desc.audio.push_back({i, 24 + (i % 4)});
		for (int i = 0; i < 4; ++i)
			desc.audio.push_back({24 + i, 28});
		const CompiledGraph graph(desc, g_sampleRate, g_maxFrames);
		std::vector<float> result;
		for (int block = 0; block < 50; ++block)
		{
			graph.process(g_maxFrames, TransportBlock{}, pool);
			for (int node = 0; node < static_cast<int>(graph.size()); ++node)
				for (int c = 0; c < 2; ++c)
					result.insert(result.end(), graph.audio(node)[c], graph.audio(node)[c] + g_maxFrames);
		}
		return result;
	}
}

TEST_CASE("subnormal numbers render the same with any number of threads", "[unit]")
{
	// Without the same floating-point mode on every thread, a node gives different bits depending on the thread that
	// runs it. The graph flushes subnormal numbers to zero, on the audio thread too.
	const std::vector<float> serial = renderTails(nullptr);
	CHECK(std::none_of(serial.begin(), serial.end(), [](float x) { return std::fpclassify(x) == FP_SUBNORMAL; }));
	CHECK(std::any_of(serial.begin(), serial.end(), [](float x) { return x != 0.0f; }));
	for (const size_t workers : {1, 3, 7})
	{
		CAPTURE(workers);
		gnubase::rt::WorkerPool pool(workers);
		for (int run = 0; run < 5; ++run)
		{
			const std::vector<float> parallel = renderTails(&pool);
			REQUIRE(parallel.size() == serial.size());
			CHECK(std::memcmp(parallel.data(), serial.data(), serial.size() * sizeof(float)) == 0);
		}
	}
}

TEST_CASE("a graph's width is the most nodes it can process at once", "[unit]")
{
	const auto node = [] { return NodeDesc{NodeKind::Channel, 0, 2, nullptr}; };
	GraphDesc chain;
	for (int i = 0; i < 5; ++i)
		chain.nodes.push_back(node());
	for (int i = 0; i < 4; ++i)
		chain.audio.push_back({i, i + 1});
	CHECK(CompiledGraph(chain, g_sampleRate, g_maxFrames).width() == 1);

	// An input bus feeding six channels, two of them through a group, into an output bus.
	GraphDesc song;
	for (int i = 0; i < 9; ++i)
		song.nodes.push_back(node());
	for (int i = 1; i <= 6; ++i)
		song.audio.push_back({0, i});
	song.audio.push_back({1, 7});
	song.audio.push_back({2, 7});
	for (int i = 3; i <= 7; ++i)
		song.audio.push_back({i, 8});
	CHECK(CompiledGraph(song, g_sampleRate, g_maxFrames).width() == 6);

	CHECK(CompiledGraph(GraphDesc{}, g_sampleRate, g_maxFrames).width() == 0);
}

namespace
{
	Json run(Client& client, const std::string& command, const Json& parameters = Json::object())
	{
		const Result result = client.call(command, parameters);
		INFO(command << ": " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, const Json& parameters)
	{
		const Result result = client.call(command, parameters);
		REQUIRE(!result.ok());
		return result.error;
	}

	// Tracks of the test synth playing chords into a group, rendered with the given number of threads.
	std::vector<float> renderSong(int threads)
	{
		Engine engine;
		Client client(engine);
		run(client, "audio.setDevice", {{"blockSize", 96}, {"inputChannels", 0}, {"captureSeconds", 1}});
		run(client, "audio.setProcessing", {{"threads", threads}});
		const Json group = run(client, "track.add", {{"type", "group"}});
		const Json project = run(client, "project.get");
		ObjectId groupChannel = 0;
		for (const Json& track : project.at("tracks"))
			if (track.at("id") == group.at("track").at("id"))
				groupChannel = track.at("channel").get<ObjectId>();
		for (int t = 0; t < 12; ++t)
		{
			const Json track = run(client, "track.add", {{"type", "instrument"}});
			const ObjectId id = track.at("track").at("id").get<ObjectId>();
			const Json tracks = run(client, "project.get").at("tracks");
			ObjectId channel = 0;
			for (const Json& item : tracks)
				if (item.at("id").get<ObjectId>() == id)
					channel = item.at("channel").get<ObjectId>();
			run(client, "channel.set", {{"channel", channel}, {"instrument", "testSynth"}, {"output", groupChannel}});
			const Json part = run(client, "midiPart.add", {{"track", id}, {"position", 0}, {"length", 1720320 * 4}});
			Json notes = Json::array();
			for (int n = 0; n < 8; ++n)
				notes.push_back({{"position", n * 215040}, {"length", 300000}, {"pitch", 40 + t + (n * 3)},
					{"velocity", 0.3 + (t * 0.05)}});
			run(client, "note.add", {{"part", part.at("part").at("id")}, {"notes", notes}});
		}
		run(client, "transport.play");
		run(client, "audio.render", {{"frames", 24000}});
		std::vector<float> samples;
		for (const int channel : {0, 1})
		{
			const auto captured = run(client, "audio.capture", {{"channel", channel}, {"start", 0}, {"frames", 24000}})
									  .at("samples")
									  .get<std::vector<float>>();
			samples.insert(samples.end(), captured.begin(), captured.end());
		}
		return samples;
	}
}

TEST_CASE("audio.setProcessing chooses the number of processing threads", "[unit]")
{
	Engine engine;
	Client client(engine);
	const size_t hardware = gnubase::rt::hardwareThreads();
	const Json status = run(client, "audio.get");
	// By default about one thread per core: the second hardware thread of a core adds little.
	CHECK(status.at("processing").at("threads").get<size_t>() == std::max<size_t>(hardware / 2, 1));

	const Json changed = run(client, "audio.setProcessing", {{"threads", 3}});
	CHECK(changed.at("processing").at("threads") == 3);
	CHECK(run(client, "audio.get").at("processing").at("threads") == 3);
	CHECK(changed.at("processing").at("activeThreads") == std::min<size_t>(3, hardware));
	run(client, "audio.setProcessing", {{"multiProcessing", false}});
	CHECK(run(client, "audio.get").at("processing") ==
		Json{{"multiProcessing", false}, {"threads", 3}, {"activeThreads", 1}, {"limitedBy", "multiProcessing"}});

	// More threads than the hardware has would only make them wait for each other: the setting stays, but at most
	// one thread per hardware thread processes.
	run(client, "audio.setProcessing", {{"multiProcessing", true}, {"threads", 64}});
	const Json many = run(client, "audio.get").at("processing");
	CHECK(many.at("threads") == 64);
	CHECK(many.at("activeThreads") == std::min<size_t>(64, hardware));
	CHECK(many.at("limitedBy") == "hardware");
	run(client, "audio.setProcessing", {{"threads", 1}});
	CHECK(run(client, "audio.get").at("processing").at("limitedBy") == nullptr);

	CHECK_THAT(reject(client, "audio.setProcessing", {{"threads", 0}}), ContainsSubstring("threads"));
	CHECK_THAT(reject(client, "audio.setProcessing", {{"threads", 65}}), ContainsSubstring("threads"));
	run(client, "audio.setDevice", {{"captureSeconds", 1}});
	run(client, "audio.start");
	CHECK_THAT(reject(client, "audio.setProcessing", {{"threads", 2}}), ContainsSubstring("running"));
	run(client, "audio.stop");
}

namespace
{
	// Simulates an OS that refuses worker pools for the lifetime of the object.
	struct Faults
	{
		explicit Faults(const gnubase::rt::WorkerPoolFaults& faults) { gnubase::rt::setWorkerPoolFaults(faults); }
		~Faults() { gnubase::rt::setWorkerPoolFaults({}); }
		Faults(const Faults&) = delete;
		Faults& operator=(const Faults&) = delete;
	};
}

TEST_CASE("only workers with real-time priority help the audio thread", "[unit]")
{
	const size_t hardware = gnubase::rt::hardwareThreads();
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice", {{"captureSeconds", 1}});
	const Json processing = run(client, "audio.setProcessing", {{"threads", 2}}).at("processing");
	// Wherever workers get real-time priority, here they do; otherwise the audio thread processes on its own.
	if (processing.at("activeThreads") == 1 && hardware > 1)
		CHECK(processing.at("limitedBy") == "realtime");

	const Faults faults({SIZE_MAX, true});
	const Json refused = run(client, "audio.setProcessing", {{"threads", 4}}).at("processing");
	CHECK(refused.at("threads") == 4);
	CHECK(refused.at("activeThreads") == 1);
	if (hardware > 1)
		CHECK(refused.at("limitedBy") == "realtime");
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 4800}}); // and it plays
}

TEST_CASE("processing threads the system refuses to start are a command error, not a crash", "[unit]")
{
	if (gnubase::rt::hardwareThreads() < 2)
		SKIP("one hardware thread: no workers to start");
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice", {{"captureSeconds", 1}});
	run(client, "audio.setProcessing", {{"threads", 1}});
	{
		const Faults faults({0, false});
		CHECK_THAT(reject(client, "audio.setProcessing", {{"threads", 2}}), ContainsSubstring("could not start"));
		const Json kept = run(client, "audio.get").at("processing"); // the previous settings
		CHECK(kept.at("threads") == 1);
		CHECK(kept.at("activeThreads") == 1);
	}
	run(client, "audio.setProcessing", {{"threads", 2}});
	{
		// A new device still opens and processes on the audio thread.
		const Faults faults({0, false});
		const Json opened = run(client, "audio.setDevice", {{"captureSeconds", 1}}).at("processing");
		CHECK(opened.at("threads") == 2);
		CHECK(opened.at("activeThreads") == 1);
		CHECK(opened.at("limitedBy") == "system");
		run(client, "transport.play");
		run(client, "audio.render", {{"frames", 4800}});
	}
}

TEST_CASE("a project renders bit-identically with one or many processing threads", "[integration]")
{
	const std::vector<float> one = renderSong(1);
	bool sound = false;
	for (const float sample : one)
		sound = sound || sample != 0.0f;
	REQUIRE(sound);
	for (const int threads : {2, 4, 8})
	{
		CAPTURE(threads);
		const std::vector<float> many = renderSong(threads);
		REQUIRE(many.size() == one.size());
		CHECK(std::memcmp(many.data(), one.data(), one.size() * sizeof(float)) == 0);
	}
}
