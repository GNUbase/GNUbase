// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/connection.h"

#include "platform/process.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <random>
#include <string>
#include <system_error>
#include <utility>

namespace gnubase::ipc
{
	namespace
	{
		constexpr auto g_none = static_cast<uint32_t>(PeerState::None);
		constexpr auto g_attached = static_cast<uint32_t>(PeerState::Attached);
		constexpr auto g_closed = static_cast<uint32_t>(PeerState::Closed);

		std::string hex(uint32_t value)
		{
			std::array<char, 8> digits{};
			const std::to_chars_result result = std::to_chars(digits.data(), digits.data() + digits.size(), value, 16);
			return {digits.data(), result.ptr};
		}
	}

	std::string makeUniqueName()
	{
		static std::atomic<uint32_t> counter{0};
		static std::random_device device;
		// "gb", the process id, a counter and a random number, in hexadecimal: at most 28 characters.
		return "gb" + hex(platform::currentProcessId()) + "-" + hex(counter.fetch_add(1)) + "-" +
			hex(static_cast<uint32_t>(device()));
	}

	Connection::~Connection()
	{
		close();
	}

	Connection::Connection(Connection&& other) noexcept
		: m_memory(std::move(other.m_memory)), m_side(other.m_side), m_config(other.m_config), m_layout(other.m_layout),
		  m_lanes(std::move(other.m_lanes)), m_engineProcess(std::move(other.m_engineProcess)),
		  m_corrupt(other.m_corrupt), m_attached(std::exchange(other.m_attached, false))
	{
	}

	Connection& Connection::operator=(Connection&& other) noexcept
	{
		if (this != &other)
		{
			close();
			m_memory = std::move(other.m_memory);
			m_side = other.m_side;
			m_config = other.m_config;
			m_layout = other.m_layout;
			m_lanes = std::move(other.m_lanes);
			m_engineProcess = std::move(other.m_engineProcess);
			m_corrupt = other.m_corrupt;
			m_attached = std::exchange(other.m_attached, false);
		}
		return *this;
	}

	Connection Connection::create(const Config& config)
	{
		rt::assertNotRealtime("Connection::create");
		if (!validConfig(config))
			throw Error("Invalid IPC configuration");
		Connection connection;
		connection.m_side = Side::Engine;
		connection.m_config = config;
		connection.m_layout = layoutFor(config);
		connection.m_memory =
			platform::SharedMemory::create(makeUniqueName(), static_cast<size_t>(connection.m_layout.size));
		initializeSegment(connection.m_memory.data(), config, platform::currentProcessId());
		connection.connect(Side::Engine);
		connection.m_attached = true;
		return connection;
	}

	Connection Connection::open(const std::string& name)
	{
		rt::assertNotRealtime("Connection::open");
		Connection connection;
		connection.m_side = Side::Host;
		try
		{
			connection.m_memory = platform::SharedMemory::open(name);
		}
		catch (const std::system_error& error)
		{
			throw Error(error.what());
		}
		const std::string problem = checkSegment(connection.m_memory.data(), connection.m_memory.size());
		if (!problem.empty())
			throw Error("Cannot connect to " + name + ": " + problem);
		// A private copy of the configuration, checked again: everything else is computed from it.
		SegmentHeader& header = connection.header();
		connection.m_config = header.config;
		connection.m_layout = layoutFor(connection.m_config);
		if (!validConfig(connection.m_config) || connection.m_layout.size > connection.m_memory.size())
			throw Error("Cannot connect to " + name + ": the configuration changed");
		connection.connect(Side::Host);
		connection.m_engineProcess = platform::ProcessWatch(header.enginePid);

		// Claim the segment before writing to it, so that a refused second host changes nothing.
		uint32_t expected = 0;
		if (!header.hostPid.compare_exchange_strong(expected, platform::currentProcessId(), std::memory_order_acq_rel))
			throw Error("Cannot connect to " + name + ": another host is attached");
		header.hostVersionMajor = g_versionMajor;
		header.hostVersionMinor = g_versionMinor;
		header.hostState.store(g_attached, std::memory_order_release);
		connection.m_attached = true;
		connection.outgoing().controlBell.ring();
		platform::endWithParent(header.enginePid);
		return connection;
	}

	void Connection::connect(Side side)
	{
		auto* bytes = static_cast<std::byte*>(m_memory.data());
		auto* audio = reinterpret_cast<AudioLane*>(bytes + m_layout.lanes);
		auto* control = reinterpret_cast<ControlLane*>(bytes + m_layout.lanes + (2 * sizeof(AudioLane)));
		for (uint32_t index = 0; index < 2; ++index)
		{
			Lane& lane = m_lanes[index];
			lane.blocks =
				BlockRing(audio[index].blocks, bytes + m_layout.blocks[index], m_layout.slotSize, m_config.blockSlots);
			lane.events = MessageRing(audio[index].events, bytes + m_layout.events[index], m_config.eventRingSize);
			lane.control =
				MessageRing(control[index].messages, bytes + m_layout.control[index], m_config.controlRingSize);
			// Windows names its wake-up events after the connection; elsewhere the names are not used.
			const std::string prefix = name() + (index == g_toHost ? "-h" : "-e");
			if (side == Side::Engine)
			{
				lane.audioBell = Doorbell::create(audio[index].wake.value, prefix + "a");
				lane.controlBell = Doorbell::create(control[index].wake.value, prefix + "c");
			}
			else
			{
				lane.audioBell = Doorbell::open(audio[index].wake.value, prefix + "a");
				lane.controlBell = Doorbell::open(control[index].wake.value, prefix + "c");
			}
		}
	}

	std::atomic<uint32_t>& Connection::ownState() const noexcept
	{
		return m_side == Side::Engine ? header().engineState : header().hostState;
	}

	const std::atomic<uint32_t>& Connection::peerStateWord() const noexcept
	{
		return m_side == Side::Engine ? header().hostState : header().engineState;
	}

	bool Connection::waitForPeer(std::chrono::nanoseconds timeout)
	{
		rt::assertNotRealtime("Connection::waitForPeer");
		const Doorbell& bell = incoming().controlBell;
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		for (;;)
		{
			const uint32_t seen = bell.value();
			if (peerStateWord().load(std::memory_order_acquire) != g_none)
			{
				// Nobody else needs the name now; on POSIX it would otherwise outlive a crashed engine.
				m_memory.removeName();
				return true;
			}
			const auto remaining = deadline - std::chrono::steady_clock::now();
			if (remaining <= std::chrono::nanoseconds::zero())
				return false;
			bell.wait(seen, remaining);
		}
	}

	PeerState Connection::peerState() const noexcept
	{
		const uint32_t state = peerStateWord().load(std::memory_order_acquire);
		return state <= g_closed ? static_cast<PeerState>(state) : PeerState::Closed;
	}

	uint32_t Connection::peerProcessId() const noexcept
	{
		return m_side == Side::Engine ? header().hostPid.load(std::memory_order_acquire) : header().enginePid;
	}

	bool Connection::beginBlock(AudioBlock& block) noexcept
	{
		std::byte* slot = outgoing().blocks.beginWrite();
		if (slot == nullptr)
			return false;
		block = AudioBlock{};
		block.slot = slot;
		block.samples = slot + sizeof(BlockHeader);
		block.channelStride = m_layout.channelStride;
		return true;
	}

	void Connection::sendBlock(const AudioBlock& block) noexcept
	{
		BlockHeader header;
		header.sequence = block.sequence;
		header.position = block.position;
		header.frames = std::min(block.frames, m_config.maxFrames);
		header.channels = std::min(block.channels, m_config.maxChannels);
		header.eventCount = block.eventCount;
		header.flags = block.flags;
		std::memcpy(block.slot, &header, sizeof(header));
		Lane& lane = outgoing();
		lane.blocks.commitWrite();
		lane.audioBell.ring();
	}

	bool Connection::sendEvent(uint16_t type, uint16_t version, const void* payload, uint32_t size) noexcept
	{
		return outgoing().events.tryWrite(type, version, payload, size);
	}

	ReadStatus Connection::receiveBlock(AudioBlock& block) noexcept
	{
		if (m_corrupt)
			return ReadStatus::Corrupt;
		BlockRing& blocks = incoming().blocks;
		std::byte* slot = blocks.beginRead();
		if (slot == nullptr)
			return blocks.corrupt() ? ReadStatus::Corrupt : ReadStatus::Empty;
		BlockHeader header;
		std::memcpy(&header, slot, sizeof(header));
		if (header.frames > m_config.maxFrames || header.channels > m_config.maxChannels)
		{
			m_corrupt = true;
			return ReadStatus::Corrupt;
		}
		block.sequence = header.sequence;
		block.position = header.position;
		block.frames = header.frames;
		block.channels = header.channels;
		block.eventCount = header.eventCount;
		block.flags = header.flags;
		block.slot = slot;
		block.samples = slot + sizeof(BlockHeader);
		block.channelStride = m_layout.channelStride;
		return ReadStatus::Ok;
	}

	void Connection::releaseBlock() noexcept
	{
		incoming().blocks.endRead();
	}

	ReadStatus Connection::receiveEvent(MessageHeader& header, void* buffer, size_t bufferSize) noexcept
	{
		return incoming().events.tryRead(header, buffer, bufferSize);
	}

	bool Connection::blockReady() noexcept
	{
		BlockRing& blocks = incoming().blocks;
		return m_corrupt || blocks.beginRead() != nullptr || blocks.corrupt();
	}

	bool Connection::answerReady(uint64_t sequence) noexcept
	{
		BlockRing& blocks = incoming().blocks;
		while (!m_corrupt)
		{
			const std::byte* slot = blocks.beginRead();
			if (slot == nullptr)
				return blocks.corrupt();
			BlockHeader header;
			std::memcpy(&header, slot, sizeof(header));
			// Counting up, wrapping around like the sequence numbers.
			const auto behind = static_cast<int64_t>(sequence - header.sequence);
			if (behind == 0)
				return true;
			if (behind < 0 || !dropEvents(header.eventCount))
				m_corrupt = true; // an answer to a block not sent yet, or events missing
			else
				blocks.endRead(); // a late answer
		}
		return true;
	}

	bool Connection::dropEvents(uint32_t count) noexcept
	{
		MessageRing& events = incoming().events;
		MessageHeader header;
		for (uint32_t i = 0; i < count; ++i)
		{
			if (events.tryRead(header, nullptr, 0) != ReadStatus::Ok)
				return false;
		}
		return true;
	}

	bool Connection::controlReady() noexcept
	{
		return incoming().control.readable();
	}

	template <typename Ready>
	WaitResult Connection::waitFor(const Doorbell& bell, Ready ready, std::chrono::nanoseconds timeout) noexcept
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		for (;;)
		{
			// Read the word first: a ring after this point ends the wait below at once.
			const uint32_t seen = bell.value();
			if (ready())
				return WaitResult::Ready;
			if (peerState() == PeerState::Closed)
				return WaitResult::PeerClosed;
			const auto remaining = deadline - std::chrono::steady_clock::now();
			if (remaining <= std::chrono::nanoseconds::zero())
			{
				// A crashed engine never closes: its host would only ever see timeouts.
				if (m_side == Side::Host && m_engineProcess.gone())
					return WaitResult::PeerClosed;
				return WaitResult::Timeout;
			}
			bell.wait(seen, remaining);
		}
	}

	WaitResult Connection::waitForBlock(std::chrono::nanoseconds timeout) noexcept
	{
		return waitFor(incoming().audioBell, [this] { return blockReady(); }, timeout);
	}

	WaitResult Connection::waitForAnswer(uint64_t sequence, std::chrono::nanoseconds timeout) noexcept
	{
		return waitFor(incoming().audioBell, [this, sequence] { return answerReady(sequence); }, timeout);
	}

	bool Connection::sendControl(uint16_t type, uint16_t version, const void* payload, uint32_t size)
	{
		rt::assertNotRealtime("Connection::sendControl");
		Lane& lane = outgoing();
		if (!lane.control.tryWrite(type, version, payload, size))
			return false;
		lane.controlBell.ring();
		return true;
	}

	ReadStatus Connection::receiveControl(MessageHeader& header, void* buffer, size_t bufferSize)
	{
		rt::assertNotRealtime("Connection::receiveControl");
		return incoming().control.tryRead(header, buffer, bufferSize);
	}

	WaitResult Connection::waitForControl(std::chrono::nanoseconds timeout)
	{
		rt::assertNotRealtime("Connection::waitForControl");
		return waitFor(incoming().controlBell, [this] { return controlReady(); }, timeout);
	}

	void Connection::close() noexcept
	{
		if (!m_attached || m_memory.data() == nullptr)
			return;
		ownState().store(g_closed, std::memory_order_release);
		Lane& lane = outgoing();
		lane.audioBell.ring();
		lane.controlBell.ring();
	}
}
