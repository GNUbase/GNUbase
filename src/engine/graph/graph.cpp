// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/graph/graph.h"

#include "engine/document/rendersnapshot.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <algorithm>
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
				bus.id, bus.channels});
		const int firstChannel = static_cast<int>(snapshot.busses.size());
		for (const RenderChannel& channel : snapshot.channels)
			graph.nodes.push_back({NodeKind::Channel, channel.id, channel.channels});

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

		for (const int index : m_order)
		{
			Node& node = m_nodes[static_cast<size_t>(index)];
			uint32_t latency = 0;
			for (const std::vector<int>* inputs : {&node.audioInputs, &node.eventInputs})
				for (const int input : *inputs)
					latency = std::max(latency, m_nodes[static_cast<size_t>(input)].pathLatency);
			node.pathLatency = latency + (node.processor != nullptr ? node.processor->latency() : 0);
		}

		m_samples.assign(channels * maxFrames, 0.0f);
		m_channels.resize(channels);
		for (size_t c = 0; c < channels; ++c)
			m_channels[c] = m_samples.data() + (c * maxFrames);
		m_eventsIn.assign(m_nodes.size(), EventBuffer(g_maxEventsPerBlock));
		m_eventsOut.assign(m_nodes.size(), EventBuffer(g_maxEventsPerBlock));
	}

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

	void CompiledGraph::process(uint32_t frames) const noexcept
	{
		GNUBASE_ASSERT(frames <= m_maxFrames);
		frames = std::min(frames, m_maxFrames);
		for (const int index : m_order)
		{
			const auto i = static_cast<size_t>(index);
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
				node.processor->process({audio, node.channels, frames, eventsIn, eventsOut});
			else
				for (const ProcessEvent& event : eventsIn)
					eventsOut.add(event);
			eventsIn.clear();
		}
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
