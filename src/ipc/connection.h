// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ipc/doorbell.h"
#include "ipc/protocol.h"
#include "ipc/rings.h"
#include "platform/sharedmemory.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace gnubase::ipc
{
	// A connection that cannot be set up: unknown name, other protocol version, invalid layout.
	class Error : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	enum class Side : uint8_t
	{
		Engine, // creates the connection and sends the blocks to process
		Host,   // the plugin host process: opens it and answers each block
	};

	enum class WaitResult : uint8_t
	{
		Ready,      // there is something to read
		Timeout,    // nothing arrived in time: the peer is slow, hung or gone
		PeerClosed, // the peer closed the connection
	};

	// A view of one audio block in shared memory. The header fields are a private copy: the reader checked them, and
	// the peer cannot change them afterwards. Samples are read and written in place.
	struct AudioBlock
	{
		uint64_t sequence = 0;
		int64_t position = 0;
		uint32_t frames = 0;
		uint32_t channels = 0;
		uint32_t eventCount = 0;
		uint32_t flags = 0;

		// `maxFrames` samples of channel `index` (below Config::maxChannels).
		float* channel(uint32_t index) const noexcept
		{
			return reinterpret_cast<float*>(samples + (size_t{index} * channelStride));
		}

		std::byte* slot = nullptr;    // the slot in the block ring
		std::byte* samples = nullptr; // channel 0
		uint32_t channelStride = 0;   // bytes from one channel to the next
	};

	// Unique name for a new connection, usable for shared memory on every platform.
	std::string makeUniqueName();

	// Shared-memory connection between the engine and one plugin host process (docs/architecture.md, "Plugin hosting
	// and IPC"). The engine creates it and starts the host with its name; the host opens it.
	//
	// The audio path is real-time safe: beginBlock, sendBlock, sendEvent, receiveBlock, releaseBlock, receiveEvent and
	// waitForBlock never allocate or lock, and waitForBlock blocks only until its deadline. Each direction has one
	// audio thread that sends and one that receives. The control path (sendControl, receiveControl, waitForControl)
	// carries versioned messages for one non-real-time thread per side. Nothing read from the peer is trusted: indices,
	// sizes and headers are checked, and a peer that breaks the protocol makes reads return ReadStatus::Corrupt.
	class Connection
	{
	public:
		Connection() = default;
		~Connection();
		Connection(Connection&& other) noexcept;
		Connection& operator=(Connection&& other) noexcept;
		Connection(const Connection&) = delete;
		Connection& operator=(const Connection&) = delete;

		// Engine: creates the shared memory and wake-up objects under a new name. Throws Error for an invalid
		// configuration, std::system_error if the system refuses.
		static Connection create(const Config& config);

		// Host: opens the connection the engine created. Throws Error if the name is unknown, the protocol version
		// differs, the memory is not a valid segment, or another host attached already.
		static Connection open(const std::string& name);

		const std::string& name() const noexcept { return m_memory.name(); }
		Side side() const noexcept { return m_side; }
		const Config& config() const noexcept { return m_config; }

		// Engine: waits until the host opened the connection. Not real-time.
		bool waitForPeer(std::chrono::nanoseconds timeout);
		PeerState peerState() const noexcept;
		// The peer's process id: the host's once it attached, the engine's for the host.
		uint32_t peerProcessId() const noexcept;

		// Audio path. beginBlock gets the next free outgoing slot (false if all are in flight), with the header zeroed;
		// fill it in, then sendBlock commits it and wakes the peer. Events of the block go out before it, with
		// sendEvent, and the block's eventCount says how many there are.
		bool beginBlock(AudioBlock& block) noexcept;
		void sendBlock(const AudioBlock& block) noexcept;
		bool sendEvent(uint16_t type, uint16_t version, const void* payload, uint32_t size) noexcept;

		// The oldest incoming block, checked against the configuration; releaseBlock hands its slot back once done.
		ReadStatus receiveBlock(AudioBlock& block) noexcept;
		void releaseBlock() noexcept;
		ReadStatus receiveEvent(MessageHeader& header, void* buffer, size_t bufferSize) noexcept;

		// Waits for an incoming block until the timeout passes or the peer closes. Ready also covers a corrupt ring,
		// which receiveBlock then reports.
		WaitResult waitForBlock(std::chrono::nanoseconds timeout) noexcept;

		// Control path, not real-time. sendControl wakes the peer's control thread.
		bool sendControl(uint16_t type, uint16_t version, const void* payload, uint32_t size);
		ReadStatus receiveControl(MessageHeader& header, void* buffer, size_t bufferSize);
		WaitResult waitForControl(std::chrono::nanoseconds timeout);

		// Tells the peer this side is gone and wakes its threads. The destructor closes as well.
		void close() noexcept;

	private:
		struct Lane
		{
			BlockRing blocks;
			MessageRing events;
			MessageRing control;
			Doorbell audioBell;
			Doorbell controlBell;
		};

		void connect(Side side);
		SegmentHeader& header() const noexcept { return *static_cast<SegmentHeader*>(m_memory.data()); }
		Lane& outgoing() noexcept { return m_lanes[m_side == Side::Engine ? g_toHost : g_toEngine]; }
		Lane& incoming() noexcept { return m_lanes[m_side == Side::Engine ? g_toEngine : g_toHost]; }
		std::atomic<uint32_t>& ownState() const noexcept;
		const std::atomic<uint32_t>& peerStateWord() const noexcept;
		WaitResult waitFor(
			const Doorbell& bell, bool (Connection::*ready)(), std::chrono::nanoseconds timeout) noexcept;
		bool blockReady() noexcept;
		bool controlReady() noexcept;

		platform::SharedMemory m_memory;
		Side m_side = Side::Engine;
		Config m_config;
		Layout m_layout;
		std::array<Lane, 2> m_lanes;
		bool m_corrupt = false;  // an incoming block header was impossible
		bool m_attached = false; // this side announced itself in the segment header, so close() tells the peer
	};
}
