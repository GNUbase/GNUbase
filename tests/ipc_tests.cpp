// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/connection.h"
#include "ipc/doorbell.h"
#include "ipc/protocol.h"
#include "ipc/rings.h"
#include "platform/sharedmemory.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sys/statvfs.h>
#endif

using namespace gnubase;
using namespace std::chrono_literals;
using Catch::Matchers::ContainsSubstring;

namespace
{
	ipc::Config smallConfig()
	{
		ipc::Config config;
		config.maxChannels = 2;
		config.maxFrames = 64;
		config.blockSlots = 4;
		config.eventRingSize = 1024;
		config.controlRingSize = 1024;
		return config;
	}

	// Ring storage outside shared memory, for testing the rings on their own.
	struct MessageRingStorage
	{
		explicit MessageRingStorage(uint32_t capacity) : data(capacity), ring(indices, data.data(), capacity) {}
		ipc::RingIndices indices;
		std::vector<std::byte> data;
		ipc::MessageRing ring;
	};

	struct BlockRingStorage
	{
		BlockRingStorage(uint32_t slotSize, uint32_t slotCount)
			: data(size_t{slotSize} * slotCount), ring(indices, data.data(), slotSize, slotCount)
		{
		}
		ipc::RingIndices indices;
		std::vector<std::byte> data;
		ipc::BlockRing ring;
	};

	// Memory aligned like a shared memory segment.
	struct alignas(64) CacheLine
	{
		std::array<std::byte, 64> bytes;
	};

	// 32-bit random numbers on every platform (mt19937's result type is 64 bits wide on some).
	class Random
	{
	public:
		explicit Random(uint32_t seed) : m_engine(seed) {}
		uint32_t operator()() { return static_cast<uint32_t>(m_engine()); }

	private:
		std::mt19937 m_engine;
	};

	bool writeValue(ipc::MessageRing& ring, uint16_t type, uint32_t value)
	{
		return ring.tryWrite(type, 1, &value, sizeof(value));
	}
}

TEST_CASE("shared memory created by one side is visible through a second mapping", "[unit]")
{
	const std::string name = ipc::makeUniqueName();
	const platform::SharedMemory created = platform::SharedMemory::create(name, 10000);
	REQUIRE(created.size() >= 10000);
	const platform::SharedMemory opened = platform::SharedMemory::open(name);
	REQUIRE(opened.size() >= 10000);
	CHECK(static_cast<const unsigned char*>(created.data())[9999] == 0);

	std::memcpy(created.data(), "hello", 6);
	CHECK(std::string(static_cast<const char*>(opened.data())) == "hello");
}

TEST_CASE("shared memory names are unique and cannot be created twice", "[unit]")
{
	const std::string name = ipc::makeUniqueName();
	CHECK(name != ipc::makeUniqueName());
	CHECK(name.size() <= platform::SharedMemory::g_maxNameLength);
	const platform::SharedMemory created = platform::SharedMemory::create(name, 4096);
	CHECK_THROWS(platform::SharedMemory::create(name, 4096));
	CHECK_THROWS(platform::SharedMemory::open(ipc::makeUniqueName()));
	CHECK_THROWS(platform::SharedMemory::create("not/a valid name", 4096));
}

TEST_CASE("a message ring passes messages in order with their type and version", "[unit]")
{
	MessageRingStorage storage(256);
	ipc::MessageRing& ring = storage.ring;
	CHECK(ring.maxMessageSize() == 120);

	const std::string text = "note on";
	REQUIRE(ring.tryWrite(7, 3, text.data(), static_cast<uint32_t>(text.size())));
	REQUIRE(writeValue(ring, 8, 42));

	std::array<std::byte, 120> buffer{};
	ipc::MessageHeader header;
	REQUIRE(ring.tryRead(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Ok);
	CHECK(header.type == 7);
	CHECK(header.version == 3);
	CHECK(std::string(reinterpret_cast<const char*>(buffer.data()), header.size) == text);
	REQUIRE(ring.tryRead(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Ok);
	CHECK(header.type == 8);
	uint32_t value = 0;
	std::memcpy(&value, buffer.data(), sizeof(value));
	CHECK(value == 42);
	CHECK(ring.tryRead(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Empty);
}

TEST_CASE("a message ring refuses messages that do not fit", "[unit]")
{
	MessageRingStorage storage(256);
	ipc::MessageRing& ring = storage.ring;
	const std::vector<std::byte> tooLarge(ring.maxMessageSize() + 1);
	CHECK(!ring.tryWrite(1, 1, tooLarge.data(), static_cast<uint32_t>(tooLarge.size())));
	CHECK(!ring.tryWrite(0, 1, nullptr, 0)); // type 0 is reserved for padding

	// 16 bytes per message with its header: the ring holds exactly 16 of them.
	int written = 0;
	while (writeValue(ring, 1, static_cast<uint32_t>(written)))
		++written;
	CHECK(written == 16);

	std::array<std::byte, 120> buffer{};
	ipc::MessageHeader header;
	REQUIRE(ring.tryRead(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Ok);
	CHECK(writeValue(ring, 1, 99));
}

TEST_CASE("a message ring wraps around with messages of any size", "[unit]")
{
	MessageRingStorage storage(512);
	ipc::MessageRing& ring = storage.ring;
	Random random(1234);
	std::deque<std::vector<std::byte>> expected;
	std::vector<std::byte> buffer(ring.maxMessageSize());
	uint32_t counter = 0;
	for (int step = 0; step < 20000; ++step)
	{
		if (random() % 2 == 0)
		{
			std::vector<std::byte> message(random() % (ring.maxMessageSize() + 1));
			for (std::byte& value : message)
				value = static_cast<std::byte>(counter++);
			if (ring.tryWrite(5, 1, message.data(), static_cast<uint32_t>(message.size())))
				expected.push_back(message);
		}
		else
		{
			ipc::MessageHeader header;
			const ipc::ReadStatus status = ring.tryRead(header, buffer.data(), buffer.size());
			if (expected.empty())
			{
				REQUIRE(status == ipc::ReadStatus::Empty);
				continue;
			}
			REQUIRE(status == ipc::ReadStatus::Ok);
			REQUIRE(header.size == expected.front().size());
			REQUIRE(std::equal(expected.front().begin(), expected.front().end(), buffer.begin()));
			expected.pop_front();
		}
	}
}

TEST_CASE("a message ring passes messages between two threads", "[unit]")
{
	MessageRingStorage storage(1024);
	ipc::MessageRing& ring = storage.ring;
	constexpr uint32_t count = 100000;
	std::thread producer(
		[&ring]
		{
			for (uint32_t i = 0; i < count;)
			{
				// Messages of 4 to 40 bytes, each filled with its number.
				std::array<uint32_t, 10> values{};
				values.fill(i);
				if (ring.tryWrite(1, 1, values.data(), static_cast<uint32_t>(sizeof(uint32_t) * (1 + (i % 10)))))
					++i;
				else
					std::this_thread::yield();
			}
		});
	std::array<uint32_t, 10> values{};
	bool inOrder = true;
	for (uint32_t i = 0; i < count && inOrder;)
	{
		ipc::MessageHeader header;
		const ipc::ReadStatus status = ring.tryRead(header, values.data(), static_cast<uint32_t>(sizeof(values)));
		REQUIRE(status != ipc::ReadStatus::Corrupt);
		if (status == ipc::ReadStatus::Empty)
		{
			std::this_thread::yield();
			continue;
		}
		inOrder =
			header.size == sizeof(uint32_t) * (1 + (i % 10)) && values[0] == i && values[(header.size / 4) - 1] == i;
		++i;
	}
	producer.join();
	CHECK(inOrder);
}

TEST_CASE("a block ring hands out its slots in order", "[unit]")
{
	BlockRingStorage storage(64, 4);
	ipc::BlockRing& ring = storage.ring;
	CHECK(ring.beginRead() == nullptr);
	for (int i = 0; i < 4; ++i)
	{
		std::byte* slot = ring.beginWrite();
		REQUIRE(slot != nullptr);
		slot[0] = static_cast<std::byte>(i);
		ring.commitWrite();
	}
	CHECK(ring.beginWrite() == nullptr);
	for (int i = 0; i < 4; ++i)
	{
		const std::byte* slot = ring.beginRead();
		REQUIRE(slot != nullptr);
		CHECK(slot[0] == static_cast<std::byte>(i));
		ring.endRead();
	}
	CHECK(ring.beginRead() == nullptr);
	CHECK(!ring.corrupt());
}

TEST_CASE("a block ring passes blocks between two threads", "[unit]")
{
	BlockRingStorage storage(256, 2);
	ipc::BlockRing& ring = storage.ring;
	constexpr uint32_t count = 50000;
	std::thread producer(
		[&ring]
		{
			for (uint32_t i = 0; i < count;)
			{
				if (std::byte* slot = ring.beginWrite())
				{
					for (size_t offset = 0; offset < 256; offset += sizeof(i))
						std::memcpy(slot + offset, &i, sizeof(i));
					ring.commitWrite();
					++i;
				}
				else
				{
					std::this_thread::yield();
				}
			}
		});
	bool intact = true;
	for (uint32_t i = 0; i < count && intact;)
	{
		const std::byte* slot = ring.beginRead();
		if (slot == nullptr)
		{
			std::this_thread::yield();
			continue;
		}
		for (size_t offset = 0; offset < 256; offset += sizeof(i))
		{
			uint32_t value = 0;
			std::memcpy(&value, slot + offset, sizeof(value));
			intact = intact && value == i;
		}
		ring.endRead();
		++i;
	}
	producer.join();
	CHECK(intact);
}

TEST_CASE("rings never read outside their memory, whatever the peer wrote", "[unit]")
{
	// Protocol fuzzing: the peer may be a crashed or misbehaving plugin host, so readers check every index and size.
	Random random(62);
	int corrupt = 0;
	for (int iteration = 0; iteration < 20000; ++iteration)
	{
		MessageRingStorage storage(128);
		for (std::byte& value : storage.data)
			value = static_cast<std::byte>(random() % 4 == 0 ? random() % 256 : random() % 16);
		// Mostly plausible indices, sometimes anything.
		const uint32_t head = random() % 8 == 0 ? random() : random() % 160;
		storage.indices.head.store(head);
		std::array<std::byte, 56> buffer{};
		ipc::MessageHeader header;
		for (int read = 0; read < 20; ++read)
		{
			const ipc::ReadStatus status = storage.ring.tryRead(header, buffer.data(), buffer.size());
			if (status == ipc::ReadStatus::Ok)
				REQUIRE(header.size <= storage.ring.maxMessageSize());
			if (status == ipc::ReadStatus::Corrupt)
			{
				++corrupt;
				CHECK(storage.ring.tryRead(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Corrupt);
				break;
			}
			if (status == ipc::ReadStatus::Empty)
				break;
		}

		BlockRingStorage blocks(64, 4);
		blocks.indices.head.store(random());
		blocks.indices.tail.store(random());
		for (int read = 0; read < 8; ++read)
		{
			if (blocks.ring.beginRead() == nullptr)
				break;
			blocks.ring.endRead();
		}
		if (blocks.ring.beginWrite() != nullptr)
			blocks.ring.commitWrite();
	}
	CHECK(corrupt > 1000);
}

TEST_CASE("a doorbell wakes a waiter in another thread", "[unit]")
{
	const std::string name = ipc::makeUniqueName();
	std::atomic<uint32_t> word{0};
	ipc::Doorbell ringer = ipc::Doorbell::create(word, name);
	const ipc::Doorbell waiter = ipc::Doorbell::open(word, name);

	SECTION("a ring before the wait is not lost")
	{
		const uint32_t seen = waiter.value();
		ringer.ring();
		CHECK(waiter.wait(seen, 10s));
	}
	SECTION("a ring during the wait ends it")
	{
		const uint32_t seen = waiter.value();
		std::thread thread(
			[&ringer]
			{
				std::this_thread::sleep_for(20ms);
				ringer.ring();
			});
		const auto start = std::chrono::steady_clock::now();
		CHECK(waiter.wait(seen, 10s));
		CHECK(std::chrono::steady_clock::now() - start < 5s);
		thread.join();
	}
	SECTION("without a ring the wait times out")
	{
		const auto start = std::chrono::steady_clock::now();
		CHECK(!waiter.wait(waiter.value(), 30ms));
		CHECK(std::chrono::steady_clock::now() - start >= 30ms);
		CHECK(!waiter.wait(waiter.value(), 0ms));
	}
}

TEST_CASE("a doorbell wait ends close to its deadline", "[unit]")
{
	// At 128 frames and 48 kHz a block lasts 2.67 ms, so a wait that overshoots by a timer tick (15.6 ms by default on
	// Windows) stalls the whole mix. The median keeps a busy CI machine's hiccups out.
	const std::string name = ipc::makeUniqueName();
	std::atomic<uint32_t> word{0};
	const ipc::Doorbell doorbell = ipc::Doorbell::create(word, name);
	std::vector<double> milliseconds;
	for (int i = 0; i < 21; ++i)
	{
		const auto start = std::chrono::steady_clock::now();
		CHECK(!doorbell.wait(doorbell.value(), 2ms));
		milliseconds.push_back(
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
	}
	std::sort(milliseconds.begin(), milliseconds.end());
	CHECK(milliseconds.front() >= 2.0);
	CHECK(milliseconds[milliseconds.size() / 2] < 7.0);
}

TEST_CASE("the shared memory layout is the same for every process", "[unit]")
{
	// 32-bit and 64-bit, x86 and ARM host processes use one layout: fixed sizes, no pointers.
	CHECK(sizeof(ipc::SegmentHeader) == 128);
	CHECK(offsetof(ipc::SegmentHeader, config) == 24);
	CHECK(offsetof(ipc::SegmentHeader, enginePid) == 48);
	CHECK(offsetof(ipc::SegmentHeader, engineState) == 60);
	CHECK(sizeof(ipc::RingIndices) == 128);
	CHECK(sizeof(ipc::AudioLane) == 320);
	CHECK(sizeof(ipc::ControlLane) == 192);
	CHECK(sizeof(ipc::BlockHeader) == 64);
	CHECK(sizeof(ipc::MessageHeader) == 8);

	const ipc::Layout layout = ipc::layoutFor(smallConfig());
	CHECK(layout.slotSize == 64 + (2 * 64 * 4));
	CHECK(layout.lanes == 128);
	CHECK(layout.size % 64 == 0);
}

TEST_CASE("invalid configurations are refused", "[unit]")
{
	ipc::Config config = smallConfig();
	CHECK(ipc::validConfig(config));
	config.eventRingSize = 1000; // not a power of two
	CHECK(!ipc::validConfig(config));
	config = smallConfig();
	config.maxChannels = 0;
	CHECK(!ipc::validConfig(config));
	config = smallConfig();
	config.maxFrames = 1 << 20;
	CHECK(!ipc::validConfig(config));
	CHECK_THROWS_AS(ipc::Connection::create(config), ipc::Error);
}

TEST_CASE("an engine and a host exchange audio blocks and events", "[unit]")
{
	ipc::Connection engine = ipc::Connection::create(smallConfig());
	CHECK(engine.side() == ipc::Side::Engine);
	CHECK(!engine.waitForPeer(0ms));
	ipc::Connection host = ipc::Connection::open(engine.name());
	CHECK(host.side() == ipc::Side::Host);
	REQUIRE(engine.waitForPeer(1s));
	CHECK(engine.peerProcessId() == host.peerProcessId());
	CHECK(host.config().maxFrames == 64);

	// The engine sends a block with two events.
	ipc::AudioBlock block;
	REQUIRE(engine.beginBlock(block));
	block.sequence = 17;
	block.position = 48000;
	block.frames = 32;
	block.channels = 2;
	for (uint32_t i = 0; i < 32; ++i)
	{
		block.channel(0)[i] = static_cast<float>(i);
		block.channel(1)[i] = -static_cast<float>(i);
	}
	const uint32_t first = 1;
	const uint32_t second = 2;
	REQUIRE(engine.sendEvent(10, 1, &first, sizeof(first)));
	REQUIRE(engine.sendEvent(11, 1, &second, sizeof(second)));
	block.eventCount = 2;
	engine.sendBlock(block);

	// The host processes it and answers.
	REQUIRE(host.waitForBlock(1s) == ipc::WaitResult::Ready);
	ipc::AudioBlock input;
	REQUIRE(host.receiveBlock(input) == ipc::ReadStatus::Ok);
	CHECK(input.sequence == 17);
	CHECK(input.position == 48000);
	CHECK(input.frames == 32);
	CHECK(input.channels == 2);
	CHECK(input.eventCount == 2);
	std::array<std::byte, 64> buffer{};
	ipc::MessageHeader header;
	REQUIRE(host.receiveEvent(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Ok);
	CHECK(header.type == 10);
	REQUIRE(host.receiveEvent(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Ok);
	CHECK(header.type == 11);
	CHECK(host.receiveEvent(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Empty);

	ipc::AudioBlock output;
	REQUIRE(host.beginBlock(output));
	output.sequence = input.sequence;
	output.frames = input.frames;
	output.channels = 1;
	for (uint32_t i = 0; i < input.frames; ++i)
		output.channel(0)[i] = input.channel(0)[i] + input.channel(1)[i] + 0.5f;
	host.releaseBlock();
	host.sendBlock(output);

	REQUIRE(engine.waitForBlock(1s) == ipc::WaitResult::Ready);
	ipc::AudioBlock result;
	REQUIRE(engine.receiveBlock(result) == ipc::ReadStatus::Ok);
	CHECK(result.sequence == 17);
	CHECK(result.channels == 1);
	CHECK(result.channel(0)[31] == 0.5f);
	engine.releaseBlock();
	CHECK(engine.receiveBlock(result) == ipc::ReadStatus::Empty);
}

TEST_CASE("control messages pass in both directions", "[unit]")
{
	ipc::Connection engine = ipc::Connection::create(smallConfig());
	ipc::Connection host = ipc::Connection::open(engine.name());
	const std::string request = "load plugin";
	REQUIRE(engine.sendControl(100, 2, request.data(), static_cast<uint32_t>(request.size())));
	REQUIRE(host.waitForControl(1s) == ipc::WaitResult::Ready);
	std::vector<std::byte> buffer(host.config().controlRingSize);
	ipc::MessageHeader header;
	REQUIRE(host.receiveControl(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Ok);
	CHECK(header.type == 100);
	CHECK(header.version == 2);
	CHECK(std::string(reinterpret_cast<const char*>(buffer.data()), header.size) == request);

	REQUIRE(host.sendControl(101, 1, nullptr, 0));
	REQUIRE(engine.waitForControl(1s) == ipc::WaitResult::Ready);
	REQUIRE(engine.receiveControl(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Ok);
	CHECK(header.type == 101);
	CHECK(header.size == 0);
}

TEST_CASE("waiting for a block ends at the deadline or when the peer closes", "[unit]")
{
	ipc::Connection engine = ipc::Connection::create(smallConfig());
	ipc::Connection host = ipc::Connection::open(engine.name());

	SECTION("a host that does not answer misses the deadline")
	{
		const auto start = std::chrono::steady_clock::now();
		CHECK(engine.waitForBlock(20ms) == ipc::WaitResult::Timeout);
		CHECK(std::chrono::steady_clock::now() - start >= 20ms);
		CHECK(engine.peerState() == ipc::PeerState::Attached);
	}
	SECTION("a host that closes ends the wait at once")
	{
		std::thread thread(
			[&host]
			{
				std::this_thread::sleep_for(20ms);
				host.close();
			});
		const auto start = std::chrono::steady_clock::now();
		CHECK(engine.waitForBlock(10s) == ipc::WaitResult::PeerClosed);
		CHECK(engine.waitForControl(10s) == ipc::WaitResult::PeerClosed);
		CHECK(std::chrono::steady_clock::now() - start < 5s);
		CHECK(engine.peerState() == ipc::PeerState::Closed);
		thread.join();
	}
	SECTION("a closing engine ends the host's wait")
	{
		engine.close();
		CHECK(host.waitForBlock(10s) == ipc::WaitResult::PeerClosed);
	}
}

TEST_CASE("late answers to blocks whose deadline passed are dropped", "[unit]")
{
	ipc::Connection engine = ipc::Connection::create(smallConfig());
	ipc::Connection host = ipc::Connection::open(engine.name());
	std::array<std::byte, 64> buffer{};
	ipc::MessageHeader header;

	// The host answers with one event of the given type.
	const auto answer = [&host, &buffer, &header](uint64_t sequence, uint16_t eventType)
	{
		ipc::AudioBlock input;
		REQUIRE(host.receiveBlock(input) == ipc::ReadStatus::Ok);
		for (uint32_t i = 0; i < input.eventCount; ++i)
			REQUIRE(host.receiveEvent(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Ok);
		host.releaseBlock();
		ipc::AudioBlock output;
		REQUIRE(host.beginBlock(output));
		output.sequence = sequence;
		output.frames = 8;
		output.channels = 1;
		output.channel(0)[0] = static_cast<float>(sequence);
		REQUIRE(host.sendEvent(eventType, 1, &sequence, sizeof(sequence)));
		output.eventCount = 1;
		host.sendBlock(output);
	};
	const auto send = [&engine](uint64_t sequence)
	{
		ipc::AudioBlock block;
		REQUIRE(engine.beginBlock(block));
		block.sequence = sequence;
		block.frames = 8;
		block.channels = 1;
		engine.sendBlock(block);
	};

	// Block 1 misses its deadline, and its answer only comes once the engine has sent block 2.
	send(1);
	CHECK(engine.waitForAnswer(1, 10ms) == ipc::WaitResult::Timeout);
	send(2);
	answer(1, 101);

	SECTION("waitForBlock reports the late answer like any other block")
	{
		REQUIRE(engine.waitForBlock(1s) == ipc::WaitResult::Ready);
		ipc::AudioBlock block;
		REQUIRE(engine.receiveBlock(block) == ipc::ReadStatus::Ok);
		CHECK(block.sequence == 1);
		engine.releaseBlock();
	}
	SECTION("waitForAnswer drops it with its events and waits for the right one")
	{
		CHECK(engine.waitForAnswer(2, 10ms) == ipc::WaitResult::Timeout);
		answer(2, 102);
		REQUIRE(engine.waitForAnswer(2, 1s) == ipc::WaitResult::Ready);
		ipc::AudioBlock block;
		REQUIRE(engine.receiveBlock(block) == ipc::ReadStatus::Ok);
		CHECK(block.sequence == 2);
		CHECK(block.channel(0)[0] == 2.0f);
		REQUIRE(engine.receiveEvent(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Ok);
		CHECK(header.type == 102);
		CHECK(engine.receiveEvent(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Empty);
		engine.releaseBlock();
		CHECK(engine.receiveBlock(block) == ipc::ReadStatus::Empty);
	}
	SECTION("an answer to a block the engine never sent breaks the protocol")
	{
		answer(7, 107);
		REQUIRE(engine.waitForAnswer(2, 1s) == ipc::WaitResult::Ready);
		ipc::AudioBlock block;
		CHECK(engine.receiveBlock(block) == ipc::ReadStatus::Corrupt);
	}
}

TEST_CASE("a refused second host leaves the first host's process id alone", "[unit]")
{
	const ipc::Connection engine = ipc::Connection::create(smallConfig());
	const ipc::Connection first = ipc::Connection::open(engine.name());
	// The first host plays another process.
	const platform::SharedMemory memory = platform::SharedMemory::open(engine.name());
	static_cast<ipc::SegmentHeader*>(memory.data())->hostPid.store(4242);
	CHECK_THROWS_WITH(ipc::Connection::open(engine.name()), ContainsSubstring("another host"));
	CHECK(engine.peerProcessId() == 4242);
}

TEST_CASE("shared memory the system cannot back fails at creation", "[unit]")
{
	// Rather than with SIGBUS on the first touch, from an audio thread, when /dev/shm is full.
#ifdef __APPLE__
	SKIP("macOS has no way to reserve the pages of shared memory up front");
#elif defined(__linux__)
	struct statvfs status{};
	REQUIRE(statvfs("/dev/shm", &status) == 0);
	if (status.f_blocks == 0)
		SKIP("/dev/shm has no size limit");
	// Larger than all of /dev/shm, so that the kernel refuses at once instead of filling it first.
	const size_t total = size_t{status.f_blocks} * status.f_frsize;
	CHECK_THROWS_AS(
		platform::SharedMemory::create(ipc::makeUniqueName(), total + (size_t{64} << 20)), std::system_error);
#else
	// More than the page file and memory of any test machine together.
	CHECK_THROWS_AS(platform::SharedMemory::create(ipc::makeUniqueName(), size_t{1} << 42), std::system_error);
#endif
}

TEST_CASE("a host refuses shared memory of another protocol version", "[unit]")
{
	const ipc::Connection engine = ipc::Connection::create(smallConfig());
	const platform::SharedMemory memory = platform::SharedMemory::open(engine.name());
	auto* header = static_cast<ipc::SegmentHeader*>(memory.data());

	SECTION("another major version")
	{
		header->versionMajor = static_cast<uint16_t>(ipc::g_versionMajor + 1);
		CHECK_THROWS_WITH(ipc::Connection::open(engine.name()), ContainsSubstring("version"));
	}
	SECTION("a newer minor version is compatible")
	{
		header->versionMinor = static_cast<uint16_t>(ipc::g_versionMinor + 1);
		CHECK_NOTHROW(ipc::Connection::open(engine.name()));
	}
	SECTION("not a GNUbase segment")
	{
		header->magic = 0;
		CHECK_THROWS_AS(ipc::Connection::open(engine.name()), ipc::Error);
	}
}

TEST_CASE("segment headers are checked before anything else is read", "[unit]")
{
	// Protocol fuzzing for the header: random changes to a valid one are refused or harmless, never a crash.
	const ipc::Config config = smallConfig();
	const ipc::Layout layout = ipc::layoutFor(config);
	std::vector<CacheLine> segment(layout.size / sizeof(CacheLine));
	const size_t size = segment.size() * sizeof(CacheLine);
	ipc::initializeSegment(segment.data(), config, 1234);
	CHECK(ipc::checkSegment(segment.data(), size).empty());
	CHECK(!ipc::checkSegment(segment.data(), size - 64).empty());
	CHECK(!ipc::checkSegment(segment.data(), 16).empty());

	Random random(62);
	int refused = 0;
	for (int iteration = 0; iteration < 20000; ++iteration)
	{
		std::vector<CacheLine> fuzzed = segment;
		for (int change = 0; change < 3; ++change)
			fuzzed[0].bytes[random() % 64] = static_cast<std::byte>(random() % 256);
		if (!ipc::checkSegment(fuzzed.data(), size).empty())
			++refused;
	}
	CHECK(refused > 10000);
}

TEST_CASE("a peer that corrupts the ring indices is detected", "[unit]")
{
	ipc::Connection engine = ipc::Connection::create(smallConfig());
	ipc::Connection host = ipc::Connection::open(engine.name());
	const platform::SharedMemory memory = platform::SharedMemory::open(engine.name());
	const ipc::Layout layout = ipc::layoutFor(smallConfig());
	auto* lanes = reinterpret_cast<ipc::AudioLane*>(static_cast<std::byte*>(memory.data()) + layout.lanes);

	// The host's lane to the engine claims a thousand blocks.
	lanes[1].blocks.head.store(1000);
	ipc::AudioBlock block;
	CHECK(engine.waitForBlock(1s) == ipc::WaitResult::Ready);
	CHECK(engine.receiveBlock(block) == ipc::ReadStatus::Corrupt);

	// A block header with more frames than the configuration allows.
	ipc::AudioBlock output;
	REQUIRE(engine.beginBlock(output));
	output.frames = 32;
	output.channels = 1;
	engine.sendBlock(output);
	auto* blockHeader = reinterpret_cast<ipc::BlockHeader*>(static_cast<std::byte*>(memory.data()) + layout.blocks[0]);
	blockHeader->frames = 100000;
	CHECK(host.receiveBlock(block) == ipc::ReadStatus::Corrupt);
}
