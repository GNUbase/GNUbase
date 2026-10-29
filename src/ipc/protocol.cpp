// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

namespace gnubase::ipc
{
	namespace
	{
		// Large enough for any real plugin, small enough for a 32-bit host process to map.
		constexpr uint64_t g_maxSegmentSize = uint64_t{1} << 30;
		constexpr uint32_t g_maxRingSize = uint32_t{1} << 24;
		constexpr uint64_t g_lanesSize = (2 * sizeof(AudioLane)) + (2 * sizeof(ControlLane));

		constexpr bool powerOfTwo(uint32_t value)
		{
			return value != 0 && (value & (value - 1)) == 0;
		}

		constexpr uint64_t roundUp(uint64_t value, uint64_t alignment)
		{
			return (value + alignment - 1) / alignment * alignment;
		}

		bool validRingSize(uint32_t size)
		{
			return powerOfTwo(size) && size >= 256 && size <= g_maxRingSize;
		}
	}

	bool validConfig(const Config& config) noexcept
	{
		if (config.maxChannels < 1 || config.maxChannels > 256 || config.maxFrames < 1 || config.maxFrames > 65536 ||
			!powerOfTwo(config.blockSlots) || config.blockSlots > 64 || !validRingSize(config.eventRingSize) ||
			!validRingSize(config.controlRingSize))
			return false;
		return layoutFor(config).size <= g_maxSegmentSize;
	}

	Layout layoutFor(const Config& config) noexcept
	{
		Layout layout;
		layout.lanes = roundUp(sizeof(SegmentHeader), g_cacheLine);
		layout.channelStride = static_cast<uint32_t>(roundUp(uint64_t{config.maxFrames} * sizeof(float), g_cacheLine));
		layout.slotSize =
			static_cast<uint32_t>(sizeof(BlockHeader) + (uint64_t{config.maxChannels} * layout.channelStride));
		uint64_t offset = layout.lanes + g_lanesSize;
		for (uint64_t& blocks : layout.blocks)
		{
			blocks = offset;
			offset += uint64_t{layout.slotSize} * config.blockSlots;
		}
		for (uint64_t& events : layout.events)
		{
			events = offset;
			offset += config.eventRingSize;
		}
		for (uint64_t& control : layout.control)
		{
			control = offset;
			offset += config.controlRingSize;
		}
		layout.size = offset;
		return layout;
	}

	void initializeSegment(void* memory, const Config& config, uint32_t enginePid)
	{
		const Layout layout = layoutFor(config);
		auto* bytes = static_cast<std::byte*>(memory);
		std::memset(memory, 0, static_cast<size_t>(layout.lanes + g_lanesSize));
		auto* header = new (memory) SegmentHeader();
		header->segmentSize = layout.size;
		header->config = config;
		header->enginePid = enginePid;
		// One at a time: array placement new may need room for a size.
		for (size_t i = 0; i < 2; ++i)
			new (bytes + layout.lanes + (i * sizeof(AudioLane))) AudioLane();
		for (size_t i = 0; i < 2; ++i)
			new (bytes + layout.lanes + (2 * sizeof(AudioLane)) + (i * sizeof(ControlLane))) ControlLane();
		header->engineState.store(static_cast<uint32_t>(PeerState::Attached), std::memory_order_release);
	}

	std::string checkSegment(const void* memory, size_t size)
	{
		if (size < sizeof(SegmentHeader))
			return "the shared memory is smaller than a segment header";
		const auto* header = static_cast<const SegmentHeader*>(memory);
		if (header->magic != g_magic)
			return "the shared memory is not a GNUbase IPC segment";
		if (header->versionMajor != g_versionMajor)
			return "the engine uses IPC protocol version " + std::to_string(header->versionMajor) + ", this process " +
				std::to_string(g_versionMajor);
		if (header->headerSize != sizeof(SegmentHeader))
			return "the segment header has an unknown size";
		if (!validConfig(header->config))
			return "the segment's configuration is invalid";
		if (header->segmentSize != layoutFor(header->config).size || header->segmentSize > size)
			return "the segment's size does not match its configuration";
		if (header->engineState.load(std::memory_order_acquire) != static_cast<uint32_t>(PeerState::Attached))
			return "the engine is not attached to the segment";
		return {};
	}
}
