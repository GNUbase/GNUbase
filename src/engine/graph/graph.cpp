// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/graph/graph.h"

#include "engine/document/rendersnapshot.h"
#include "platform/log.h"
#include "platform/rt/denormals.h"
#include "platform/rt/mpmcqueue.h"
#include "platform/rt/realtime.h"
#include "platform/rt/workerpool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <queue>
#include <stdexcept>
#include <string>

namespace gnubase::engine::graph
{
	const char* toString(NodeKind kind)
	{
		switch (kind)
		{
		case NodeKind::InputBus:
			return "inputBus";
		case NodeKind::OutputBus:
			return "outputBus";
		case NodeKind::Channel:
			return "channel";
		case NodeKind::Builtin:
			return "builtin";
		case NodeKind::PluginSlot:
			return "pluginSlot";
		}
		return "unknown";
	}

	GraphDesc describe(const RenderSnapshot& snapshot)
	{
		GraphDesc graph;
		for (const RenderBus& bus : snapshot.busses)
			graph.nodes.push_back({bus.direction == BusDirection::Input ? NodeKind::InputBus : NodeKind::OutputBus,
				bus.id, bus.channels, nullptr});
		const int firstChannel = static_cast<int>(snapshot.busses.size());
		for (const RenderChannel& channel : snapshot.channels)
			graph.nodes.push_back({NodeKind::Channel, channel.id, channel.channels, nullptr});

		for (size_t i = 0; i < snapshot.channels.size(); ++i)
		{
			const RenderChannel& channel = snapshot.channels[i];
			const int node = firstChannel + static_cast<int>(i);
			if (channel.input >= 0)
				graph.audio.push_back({channel.input, node});
			switch (channel.output.kind)
			{
			case RenderTarget::Kind::None:
				break;
			case RenderTarget::Kind::Bus:
				graph.audio.push_back({node, channel.output.index});
				break;
			case RenderTarget::Kind::Channel:
				(channel.type == TrackType::Midi ? graph.events : graph.audio)
					.push_back({node, firstChannel + channel.output.index});
				break;
			}
		}
		return graph;
	}

	std::vector<int> processingOrder(const GraphDesc& graph)
	{
		const int size = static_cast<int>(graph.nodes.size());
		std::vector<std::vector<int>> receivers(graph.nodes.size());
		std::vector<int> pending(graph.nodes.size(), 0); // connections from nodes not yet in the order
		for (const std::vector<Connection>* connections : {&graph.audio, &graph.events})
			for (const Connection& connection : *connections)
			{
				for (const int node : {connection.from, connection.to})
					if (node < 0 || node >= size)
						throw std::invalid_argument("a connection refers to no node " + std::to_string(node));
				receivers[static_cast<size_t>(connection.from)].push_back(connection.to);
				++pending[static_cast<size_t>(connection.to)];
			}

		// Kahn's algorithm, always taking the lowest ready index.
		std::priority_queue<int, std::vector<int>, std::greater<>> ready;
		for (int node = 0; node < size; ++node)
			if (pending[static_cast<size_t>(node)] == 0)
				ready.push(node);
		std::vector<int> order;
		order.reserve(graph.nodes.size());
		while (!ready.empty())
		{
			const int node = ready.top();
			ready.pop();
			order.push_back(node);
			for (const int receiver : receivers[static_cast<size_t>(node)])
				if (--pending[static_cast<size_t>(receiver)] == 0)
					ready.push(receiver);
		}
		if (order.size() != graph.nodes.size())
			throw std::invalid_argument("the processing graph has a cycle");
		return order;
	}

	namespace
	{
		// A worker that finds no ready node for this long leaves the cycle, so it doesn't spin at real-time priority
		// while the last nodes of a narrow part of the graph run. The audio thread stays until every node is done.
		constexpr auto g_idleSpin = std::chrono::microseconds(50);
	}

	// One parallel cycle: which nodes are ready, how many senders each still waits for, and what nodes cost.
	class CompiledGraph::Scheduler final : public rt::ParallelJob
	{
	public:
		explicit Scheduler(const CompiledGraph& graph)
			: m_graph(graph), m_waiting(std::make_unique<std::atomic<int>[]>(graph.m_nodes.size())),
			  m_ready(graph.m_nodes.size()), m_cost(graph.m_nodes.size(), 0), m_rank(graph.m_nodes.size(), 0)
		{
			for (size_t i = 0; i < graph.m_nodes.size(); ++i)
				if (graph.m_nodes[i].senders == 0)
					m_roots.push_back(static_cast<int>(i));
		}

		void run(uint32_t frames, const TransportBlock& transport, rt::WorkerPool& workers, size_t helpers) noexcept
		{
			const std::vector<Node>& nodes = m_graph.m_nodes;
			m_frames = frames;
			m_transport = &transport;
			m_done.store(0, std::memory_order_relaxed);
			for (size_t i = 0; i < nodes.size(); ++i)
				m_waiting[i].store(nodes[i].senders, std::memory_order_relaxed);

			// The longest path of measured work from a node to the end goes first.
			for (auto index = m_graph.m_order.rbegin(); index != m_graph.m_order.rend(); ++index)
			{
				const auto i = static_cast<size_t>(*index);
				uint64_t after = 0;
				for (const int receiver : nodes[i].receivers)
					after = std::max(after, m_rank[static_cast<size_t>(receiver)]);
				m_rank[i] = m_cost[i] + after;
			}
			std::sort(m_roots.begin(), m_roots.end(),
				[this](int a, int b)
				{
					const uint64_t rankA = m_rank[static_cast<size_t>(a)];
					const uint64_t rankB = m_rank[static_cast<size_t>(b)];
					return rankA != rankB ? rankA > rankB : a < b;
				});
			for (const int root : m_roots)
				m_ready.tryPush(root); // never full: it holds every node
			workers.run(*this, helpers);
		}

		void work(size_t thread) noexcept override
		{
			const std::vector<Node>& nodes = m_graph.m_nodes;
			auto idleSince = std::chrono::steady_clock::time_point::max();
			while (m_done.load(std::memory_order_acquire) < nodes.size())
			{
				int index = 0;
				if (!m_ready.tryPop(index))
				{
					if (thread != 0)
					{
						const auto now = std::chrono::steady_clock::now();
						if (idleSince == std::chrono::steady_clock::time_point::max())
							idleSince = now;
						else if (now - idleSince > g_idleSpin)
							return;
					}
					rt::cpuPause();
					continue;
				}
				idleSince = std::chrono::steady_clock::time_point::max();
				const auto i = static_cast<size_t>(index);
				const auto start = std::chrono::steady_clock::now();
				m_graph.processNode(i, m_frames, *m_transport);
				const auto nanoseconds = static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
						.count());
				m_cost[i] = m_cost[i] - (m_cost[i] / 8) + (nanoseconds / 8); // a moving average over a few blocks

				// The last sender to finish makes a receiver ready; its acquire sees every sender's output.
				for (const int receiver : nodes[i].receivers)
					if (m_waiting[static_cast<size_t>(receiver)].fetch_sub(1, std::memory_order_acq_rel) == 1)
						m_ready.tryPush(receiver);
				m_done.fetch_add(1, std::memory_order_acq_rel);
			}
		}

	private:
		const CompiledGraph& m_graph;
		std::unique_ptr<std::atomic<int>[]> m_waiting; // per node, the senders not done yet
		rt::MpmcQueue<int> m_ready;
		std::atomic<size_t> m_done{0};
		uint32_t m_frames = 0;
		const TransportBlock* m_transport = nullptr;
		// Written by the thread that processes the node and read at the start of the next cycle, after run returned.
		std::vector<uint64_t> m_cost; // nanoseconds per node
		std::vector<uint64_t> m_rank; // a node's cost plus the most its receivers have ahead of them
		std::vector<int> m_roots;     // the nodes that receive from none
	};

	CompiledGraph::CompiledGraph(const GraphDesc& graph, double sampleRate, uint32_t maxFrames)
		: m_sampleRate(sampleRate), m_maxFrames(maxFrames), m_order(processingOrder(graph))
	{
		rt::assertNotRealtime("compiling a processing graph");

		size_t channels = 0;
		m_nodes.reserve(graph.nodes.size());
		for (const NodeDesc& desc : graph.nodes)
		{
			Node node;
			node.kind = desc.kind;
			node.object = desc.object;
			node.channels = std::max(desc.channels, 0);
			node.processor = desc.processor.get();
			node.firstChannel = channels;
			channels += static_cast<size_t>(node.channels);
			if (desc.processor)
			{
				desc.processor->prepareFor(sampleRate, maxFrames);
				m_processors.push_back(desc.processor);
			}
			m_nodes.push_back(std::move(node));
		}
		for (const Connection& connection : graph.audio)
			m_nodes[static_cast<size_t>(connection.to)].audioInputs.push_back(connection.from);
		for (const Connection& connection : graph.events)
			m_nodes[static_cast<size_t>(connection.to)].eventInputs.push_back(connection.from);
		for (Node& node : m_nodes)
		{
			std::sort(node.audioInputs.begin(), node.audioInputs.end());
			std::sort(node.eventInputs.begin(), node.eventInputs.end());
		}
		for (size_t i = 0; i < m_nodes.size(); ++i)
		{
			Node& node = m_nodes[i];
			std::vector<int> senders = node.audioInputs;
			senders.insert(senders.end(), node.eventInputs.begin(), node.eventInputs.end());
			std::sort(senders.begin(), senders.end());
			senders.erase(std::unique(senders.begin(), senders.end()), senders.end());
			node.senders = static_cast<int>(senders.size());
			for (const int sender : senders)
				m_nodes[static_cast<size_t>(sender)].receivers.push_back(static_cast<int>(i));
		}

		for (const int index : m_order)
		{
			Node& node = m_nodes[static_cast<size_t>(index)];
			uint32_t latency = 0;
			for (const std::vector<int>* inputs : {&node.audioInputs, &node.eventInputs})
				for (const int input : *inputs)
					latency = std::max(latency, m_nodes[static_cast<size_t>(input)].pathLatency);
			node.pathLatency = latency + (node.processor != nullptr ? node.processor->latency() : 0);
		}

		// The most nodes at one depth: they are independent, so that many threads can work at once.
		std::vector<size_t> depth(m_nodes.size(), 0);
		std::vector<size_t> atDepth(m_nodes.size() + 1, 0);
		for (const int index : m_order)
		{
			const auto i = static_cast<size_t>(index);
			for (const int receiver : m_nodes[i].receivers)
				depth[static_cast<size_t>(receiver)] = std::max(depth[static_cast<size_t>(receiver)], depth[i] + 1);
			m_width = std::max(m_width, ++atDepth[depth[i]]);
		}

		m_samples.assign(channels * maxFrames, 0.0f);
		m_channels.resize(channels);
		for (size_t c = 0; c < channels; ++c)
			m_channels[c] = m_samples.data() + (c * maxFrames);
		m_eventsIn.assign(m_nodes.size(), EventBuffer(g_maxEventsPerBlock));
		m_eventsOut.assign(m_nodes.size(), EventBuffer(g_maxEventsPerBlock));
		m_scheduler = std::make_unique<Scheduler>(*this);
	}

	CompiledGraph::~CompiledGraph() = default;

	int CompiledGraph::find(ObjectId object) const
	{
		if (object == 0)
			return -1;
		const auto node =
			std::find_if(m_nodes.begin(), m_nodes.end(), [object](const Node& n) { return n.object == object; });
		return node != m_nodes.end() ? static_cast<int>(node - m_nodes.begin()) : -1;
	}

	float* const* CompiledGraph::audio(int node) const
	{
		return m_channels.data() + m_nodes[static_cast<size_t>(node)].firstChannel;
	}

	EventBuffer& CompiledGraph::events(int node) const
	{
		return m_eventsIn[static_cast<size_t>(node)];
	}

	void CompiledGraph::process(
		uint32_t frames, const TransportBlock& transport, rt::WorkerPool* workers) const noexcept
	{
		GNUBASE_ASSERT(frames <= m_maxFrames);
		frames = std::min(frames, m_maxFrames);
		// The same mode on every thread, whichever processes a node: subnormal numbers become zero.
		const rt::ScopedDenormalsDisabled denormals;
		const size_t usable = m_width > 1 ? m_width - 1 : 0; // workers besides this thread
		const size_t helpers = workers != nullptr ? std::min(workers->workers(), usable) : 0;
		if (helpers > 0)
		{
			m_scheduler->run(frames, transport, *workers, helpers);
			return;
		}
		for (const int index : m_order)
			processNode(static_cast<size_t>(index), frames, transport);
	}

	void CompiledGraph::processNode(size_t i, uint32_t frames, const TransportBlock& transport) const noexcept
	{
		const Node& node = m_nodes[i];
		float* const* audio = m_channels.data() + node.firstChannel;
		if (node.kind != NodeKind::InputBus)
			for (int c = 0; c < node.channels; ++c)
				std::memset(audio[c], 0, frames * sizeof(float));
		for (const int input : node.audioInputs)
			mix(m_nodes[static_cast<size_t>(input)], node, frames);

		EventBuffer& eventsIn = m_eventsIn[i];
		for (const int input : node.eventInputs)
			for (const ProcessEvent& event : m_eventsOut[static_cast<size_t>(input)])
				eventsIn.add(event);

		EventBuffer& eventsOut = m_eventsOut[i];
		eventsOut.clear();
		if (node.processor != nullptr)
			node.processor->process({audio, node.channels, frames, eventsIn, eventsOut, transport});
		else
			for (const ProcessEvent& event : eventsIn)
				eventsOut.add(event);
		eventsIn.clear();
	}

	void CompiledGraph::mix(const Node& from, const Node& to, uint32_t frames) const noexcept
	{
		if (from.channels == 0)
			return;
		float* const* source = m_channels.data() + from.firstChannel;
		float* const* target = m_channels.data() + to.firstChannel;
		for (int c = 0; c < to.channels; ++c)
		{
			if (from.channels > 1 && c >= from.channels)
				break;
			const float* in = source[from.channels == 1 ? 0 : c];
			float* out = target[c];
			for (uint32_t i = 0; i < frames; ++i)
				out[i] += in[i];
		}
	}
}
