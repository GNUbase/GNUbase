// SPDX-License-Identifier: GPL-3.0-or-later
// Parallel processing (#212): the graph's nodes run on a worker pool, and the output is bit-identical for any
// number of threads.
#include "engine/engine.h"
#include "engine/graph/graph.h"
#include "platform/rt/workerpool.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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
		REQUIRE_FALSE(result.ok());
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
	const Json status = run(client, "audio.get");
	CHECK(status.at("processing").at("threads").get<int>() >= 1);
	CHECK(status.at("processing").at("threads").get<size_t>() == gnubase::rt::hardwareThreads());

	const Json changed = run(client, "audio.setProcessing", {{"threads", 3}});
	CHECK(changed.at("processing").at("threads") == 3);
	CHECK(run(client, "audio.get").at("processing").at("threads") == 3);
	run(client, "audio.setProcessing", {{"multiProcessing", false}});
	CHECK(run(client, "audio.get").at("processing") == Json{{"multiProcessing", false}, {"threads", 3}});

	CHECK_THAT(reject(client, "audio.setProcessing", {{"threads", 0}}), ContainsSubstring("threads"));
	CHECK_THAT(reject(client, "audio.setProcessing", {{"threads", 65}}), ContainsSubstring("threads"));
	run(client, "audio.setDevice", {{"captureSeconds", 1}});
	run(client, "audio.start");
	CHECK_THAT(reject(client, "audio.setProcessing", {{"threads", 2}}), ContainsSubstring("running"));
	run(client, "audio.stop");
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
