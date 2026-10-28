// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/events.h"

#include <cstddef>
#include <cstdint>

namespace gnubase::engine::graph
{
	// Frames of a block that play consecutive project positions.
	struct TransportSegment
	{
		uint32_t offset = 0;  // the segment's first frame in the block
		uint32_t frames = 0;  // at least 1
		int64_t position = 0; // the project position, in samples, of the segment's first frame
	};

	// Where a block plays in the project. While playing, the segments cover the block in order; a new segment starts
	// where cycle playback jumps from the right to the left locator. While stopped there are none.
	struct TransportBlock
	{
		bool playing = false;
		int64_t position = 0; // the project position of the block's first frame: the cursor while stopped
		const TransportSegment* segments = nullptr;
		size_t segmentCount = 0;
	};

	// What a processor gets for one block.
	struct ProcessContext
	{
		// The node's audio, processed in place: on entry the sum of the node's audio inputs (silence if it has none),
		// on return the node's output.
		float* const* audio = nullptr;
		int channels = 0;
		uint32_t frames = 0;
		const EventBuffer& eventsIn;
		EventBuffer& eventsOut; // empty on entry; events for the nodes this one has event connections to
		const TransportBlock& transport;
	};

	// Processes a node's audio and events on the audio thread: GNUbase's built-in processors (#20) and, until plugin
	// hosting (M05), placeholders for plugin slots. A processor is shared by the compiled graphs it is part of, so it
	// keeps its state when the graph is rebuilt. Only one of them processes at a time.
	class Processor
	{
	public:
		Processor() = default;
		virtual ~Processor() = default;
		Processor(const Processor&) = delete;
		Processor& operator=(const Processor&) = delete;

		// Audio thread. Real-time rules apply: no allocations, locks, blocking calls, I/O or exceptions.
		virtual void process(const ProcessContext& context) noexcept = 0;

		// The samples of delay this processor adds to its output. Read when a graph is compiled; latency
		// compensation (#71) uses it.
		virtual uint32_t latency() const { return 0; }

		// Compiling a graph calls this off the audio thread. It prepares the processor the first time and when the
		// sample rate or the maximum block size changes, which may only happen while no graph is processing.
		void prepareFor(double sampleRate, uint32_t maxFrames)
		{
			if (sampleRate == m_sampleRate && maxFrames == m_maxFrames)
				return;
			prepare(sampleRate, maxFrames);
			m_sampleRate = sampleRate;
			m_maxFrames = maxFrames;
		}

	protected:
		// Off the audio thread: allocate what process needs for blocks of up to maxFrames.
		virtual void prepare(double /*sampleRate*/, uint32_t /*maxFrames*/) {}

	private:
		double m_sampleRate = 0.0;
		uint32_t m_maxFrames = 0;
	};
}
