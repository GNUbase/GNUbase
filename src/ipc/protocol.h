// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

#ifdef _MSC_VER
// Padding from alignas is intended: data written by different processes stays on separate cache lines.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

// The binary layout of the shared memory between the engine and a plugin host process (docs/architecture.md, "Plugin
// hosting and IPC"). 32-bit and 64-bit, x86 and ARM processes map the same memory, so everything in it has a fixed
// size, offset and alignment: no pointers, no size_t, no bool, only fixed-width integers and lock-free 32-bit atomics.
// The static_asserts below pin the layout; changing it means a new major version. All supported targets are
// little-endian.
namespace gnubase::ipc
{
	inline constexpr uint32_t g_magic = 0x49424E47; // "GNBI" in memory
	// Hosts refuse another major version. Minor versions only add what older peers can ignore: new message types,
	// fields in reserved space that read as zero.
	inline constexpr uint16_t g_versionMajor = 1;
	inline constexpr uint16_t g_versionMinor = 0;
	inline constexpr uint32_t g_cacheLine = 64;

	static_assert(std::atomic<uint32_t>::is_always_lock_free && sizeof(std::atomic<uint32_t>) == 4,
		"shared memory needs address-free 32-bit atomics");

	// Sizes of one connection, chosen by the engine.
	struct Config
	{
		uint32_t maxChannels = 2;           // audio channels per block, 1 to 256
		uint32_t maxFrames = 4096;          // frames per block, 1 to 65536
		uint32_t blockSlots = 2;            // audio blocks in flight per direction: a power of two, 1 to 64
		uint32_t eventRingSize = 65536;     // bytes per direction: a power of two, 256 to 16 MiB
		uint32_t controlRingSize = 1 << 20; // bytes per direction: a power of two, 256 to 16 MiB
		uint32_t reserved = 0;
	};

	enum class PeerState : uint8_t
	{
		None = 0,     // not there yet
		Attached = 1, // connected
		Closed = 2,   // left in an orderly way; a crashed peer stays Attached
	};

	// At offset 0 of the shared memory. Written by the engine before the host opens it, except for the host fields.
	struct alignas(g_cacheLine) SegmentHeader
	{
		uint32_t magic = g_magic;
		uint16_t versionMajor = g_versionMajor;
		uint16_t versionMinor = g_versionMinor;
		uint32_t headerSize = 128; // sizeof(SegmentHeader)
		uint32_t reserved0 = 0;
		uint64_t segmentSize = 0; // bytes the layout takes (layoutFor)
		Config config;
		uint32_t enginePid = 0;
		// The host claims the segment by swapping in its process id, before it writes anything else. Reported by the
		// host itself, so never a process id to kill.
		std::atomic<uint32_t> hostPid{0};
		uint16_t hostVersionMajor = 0; // written by the host when it attaches
		uint16_t hostVersionMinor = 0;
		std::atomic<uint32_t> engineState{0}; // PeerState
		std::atomic<uint32_t> hostState{0};   // PeerState
	};

	// The read and write positions of a single-producer, single-consumer ring. They count up freely and wrap at 2^32.
	struct RingIndices
	{
		alignas(g_cacheLine) std::atomic<uint32_t> head{0}; // written by the producer only
		alignas(g_cacheLine) std::atomic<uint32_t> tail{0}; // written by the consumer only
	};

	// A word the sender counts up to wake the receiver (Doorbell).
	struct alignas(g_cacheLine) WakeWord
	{
		std::atomic<uint32_t> value{0};
	};

	// One direction of the audio path: blocks of audio and the events that belong to them.
	struct AudioLane
	{
		WakeWord wake; // the receiver's audio thread waits on it
		RingIndices blocks;
		RingIndices events;
	};

	// One direction of the control path: versioned messages that are not real-time.
	struct ControlLane
	{
		WakeWord wake; // the receiver's control thread waits on it
		RingIndices messages;
	};

	// Lane 0 goes from the engine to the host, lane 1 back.
	inline constexpr uint32_t g_toHost = 0;
	inline constexpr uint32_t g_toEngine = 1;

	// At the start of every audio block slot, followed by the channels: `maxFrames` samples each, rounded up to whole
	// cache lines (Layout::channelStride).
	struct alignas(g_cacheLine) BlockHeader
	{
		uint64_t sequence = 0;   // counts the engine's blocks; an answer repeats the number it answers
		int64_t position = 0;    // project position of the first frame, in samples
		uint32_t frames = 0;     // up to maxFrames
		uint32_t channels = 0;   // up to maxChannels
		uint32_t eventCount = 0; // events of this block, sent before it on the lane's event ring
		uint32_t flags = 0;
	};

	// Before every message in a message ring. The payload follows, padded to 8 bytes.
	struct MessageHeader
	{
		uint32_t size = 0; // payload bytes
		uint16_t type = 0; // 0 marks padding up to the end of the ring
		uint16_t version = 0;
	};

	static_assert(std::is_standard_layout_v<SegmentHeader> && sizeof(SegmentHeader) == 128);
	static_assert(offsetof(SegmentHeader, segmentSize) == 16 && offsetof(SegmentHeader, config) == 24);
	static_assert(offsetof(SegmentHeader, enginePid) == 48 && offsetof(SegmentHeader, hostPid) == 52);
	static_assert(offsetof(SegmentHeader, hostVersionMajor) == 56);
	static_assert(offsetof(SegmentHeader, engineState) == 60 && offsetof(SegmentHeader, hostState) == 64);
	static_assert(sizeof(Config) == 24 && sizeof(RingIndices) == 128 && sizeof(WakeWord) == 64);
	static_assert(sizeof(AudioLane) == 320 && sizeof(ControlLane) == 192);
	static_assert(sizeof(BlockHeader) == 64 && offsetof(BlockHeader, frames) == 16);
	static_assert(sizeof(MessageHeader) == 8);

	// Where everything is, in bytes from the start of the shared memory. Computed from the configuration the same way
	// by both processes, so the memory holds no offsets that could be wrong.
	struct Layout
	{
		uint64_t lanes = 0;    // AudioLane[2], then ControlLane[2]
		uint64_t blocks[2]{};  // per lane: blockSlots slots of slotSize bytes
		uint64_t events[2]{};  // per lane: eventRingSize bytes
		uint64_t control[2]{}; // per lane: controlRingSize bytes
		uint32_t slotSize = 0;
		uint32_t channelStride = 0; // bytes from one channel to the next in a slot
		uint64_t size = 0;
	};

	bool validConfig(const Config& config) noexcept;

	// The layout of a valid configuration.
	Layout layoutFor(const Config& config) noexcept;

	// Writes a new segment header for the configuration and constructs the lanes, in zeroed memory of layoutFor's size.
	void initializeSegment(void* memory, const Config& config, uint32_t enginePid);

	// Checks a segment header before anything else in the memory is used: magic, version, configuration and size.
	// Returns why the segment is unusable, or an empty string if it is fine.
	std::string checkSegment(const void* memory, size_t size);
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
