// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/graph/events.h"
#include "engine/graph/processor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace gnubase::engine
{
	struct RenderSnapshot;
}

namespace gnubase::rt
{
	class WorkerPool;
}

// The real-time audio processing graph. The command thread describes it from a render snapshot (describe), adds
// processors and compiles it (CompiledGraph), and hands it to the audio thread with rt::SnapshotPublisher. The audio
// thread processes one block at a time: every node after the nodes it receives from, on its own or with a worker
// pool (#212). Latency compensation follows in #71.
namespace gnubase::engine::graph
{
	enum class NodeKind : uint8_t
	{
		InputBus,   // audio from the device; written into the node's buffer before processing
		OutputBus,  // audio for the device; read from the node's buffer after processing
		Channel,    // a track's mixer channel; sums its inputs
		Builtin,    // one of GNUbase's own processors
		PluginSlot, // a third-party plugin; until plugin hosting (M05) a placeholder with a built-in processor
	};

	const char* toString(NodeKind kind);

	struct NodeDesc
	{
		NodeKind kind = NodeKind::Channel;
		ObjectId object = 0;                  // the bus or channel in the document, or 0
		int channels = 2;                     // audio channels; 0 for nodes that only pass events, like MIDI channels
		std::shared_ptr<Processor> processor; // or none: the node outputs the sum of its inputs and passes events on
	};

	// From the node at index from to the node at index to.
	struct Connection
	{
		int from = 0;
		int to = 0;

		bool operator==(const Connection& other) const { return from == other.from && to == other.to; }
	};

	struct GraphDesc
	{
		std::vector<NodeDesc> nodes;
		std::vector<Connection> audio;  // audio from one node is added to another's input
		std::vector<Connection> events; // events one node outputs reach another
	};

	// The graph of a render snapshot: a node per bus in bus order, then a node per channel in track order. Channels
	// receive audio from their input bus and send it to their output bus or group channel; MIDI channels send events
	// to their instrument channel instead.
	GraphDesc describe(const RenderSnapshot& snapshot);

	// The order in which nodes are processed: every node after the nodes it receives audio or events from, and
	// otherwise by index, so the order depends only on the graph and not on the order of its connections. Throws
	// std::invalid_argument if a connection refers to a missing node or the connections form a cycle.
	std::vector<int> processingOrder(const GraphDesc& graph);

	// A graph ready for the audio thread. The command thread compiles it, which allocates every buffer and prepares
	// the processors; processing one block then neither allocates, locks nor blocks. The structure is immutable;
	// audio and event buffers are the audio thread's scratch memory, so they can be used through a const graph, as
	// rt::SnapshotPublisher hands it out.
	//
	// A node's input is the sum of its audio inputs, added in the order of their node indices once all of them are
	// done, so the output is bit-identical for the same graph and input, however many threads process it. A mono
	// input feeds every channel of the node it is connected to; otherwise channels are connected one by one and
	// channels the other node lacks are left out.
	class CompiledGraph
	{
	public:
		// Throws std::invalid_argument like processingOrder. Prepares every processor for the sample rate and block
		// size (Processor::prepareFor).
		CompiledGraph(const GraphDesc& graph, double sampleRate, uint32_t maxFrames);
		~CompiledGraph();
		CompiledGraph(const CompiledGraph&) = delete;
		CompiledGraph& operator=(const CompiledGraph&) = delete;
		CompiledGraph(CompiledGraph&&) = delete;
		CompiledGraph& operator=(CompiledGraph&&) = delete;

		double sampleRate() const { return m_sampleRate; }
		uint32_t maxFrames() const { return m_maxFrames; }
		size_t size() const { return m_nodes.size(); }
		const std::vector<int>& order() const { return m_order; }

		// The index of the node for a bus or channel, or -1.
		int find(ObjectId object) const;

		// Samples of delay on the longest path from the graph's sources to the node's output.
		uint32_t pathLatency(int node) const { return m_nodes[static_cast<size_t>(node)].pathLatency; }

		// Audio thread. The node's audio channels, each maxFrames long. Write an input bus's audio before process;
		// read any node's output after it.
		float* const* audio(int node) const;

		// Audio thread. Events for the node in the next block, at offsets below the block's frames. Process uses them
		// up.
		EventBuffer& events(int node) const;

		// Audio thread. Processes a block of up to maxFrames frames, which plays where transport says. With workers,
		// the nodes are tasks on the pool: a node starts once the nodes it receives from are done, and of the nodes
		// that are ready at the start, those with the most work ahead of them go first, measured in earlier blocks.
		// Without workers, or with none, this thread processes the nodes in order.
		void process(uint32_t frames, const TransportBlock& transport = TransportBlock{},
			rt::WorkerPool* workers = nullptr) const noexcept;

		// Audio thread, after process: how long a thread processed nodes in the last block, in nanoseconds. Thread 0
		// is the one that called process, 1 to 64 the workers.
		uint64_t busyNanoseconds(size_t thread) const;
		static constexpr size_t g_maxThreads = 65;

	private:
		class Scheduler;

		struct Node
		{
			NodeKind kind = NodeKind::Channel;
			ObjectId object = 0;
			int channels = 0;
			Processor* processor = nullptr; // owned by m_processors
			std::vector<int> audioInputs;   // node indices, ascending
			std::vector<int> eventInputs;   // node indices, ascending
			std::vector<int> receivers;     // the nodes that receive its audio or events, ascending, once each
			int senders = 0;                // the nodes it receives from, once each
			size_t firstChannel = 0;        // into m_channels
			uint32_t pathLatency = 0;
		};

		void processNode(size_t index, uint32_t frames, const TransportBlock& transport) const noexcept;
		void mix(const Node& from, const Node& to, uint32_t frames) const noexcept;

		double m_sampleRate;
		uint32_t m_maxFrames;
		std::vector<Node> m_nodes;
		std::vector<int> m_order;
		std::vector<std::shared_ptr<Processor>> m_processors; // kept alive while the graph exists

		// The audio thread's scratch memory: sized here, never resized.
		mutable std::vector<float> m_samples;
		mutable std::vector<float*> m_channels; // into m_samples, maxFrames apart
		mutable std::vector<EventBuffer> m_eventsIn;
		mutable std::vector<EventBuffer> m_eventsOut;
		std::unique_ptr<Scheduler> m_scheduler; // the parallel cycle's state
	};

	// The most events a node receives or sends in one block; the rest are dropped.
	inline constexpr size_t g_maxEventsPerBlock = 1024;
}
