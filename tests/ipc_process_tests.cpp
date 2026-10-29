// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/connection.h"
#include "platform/process.h"
#include "platform/sharedmemory.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

// CMake passes the helper's full path; the fallback only serves tools that compile this file on its own.
#ifndef GNUBASE_IPCPEER
#define GNUBASE_IPCPEER "gnubase-ipcpeer"
#endif

using namespace gnubase;
using namespace std::chrono_literals;

namespace
{
	ipc::Config config()
	{
		ipc::Config result;
		result.maxChannels = 2;
		result.maxFrames = 256;
		result.blockSlots = 2;
		result.eventRingSize = 4096;
		result.controlRingSize = 4096;
		return result;
	}

	// gnubase-ipcpeer plays the plugin host process (tests/ipcpeer.cpp).
	platform::ChildProcess startPeer(const ipc::Connection& connection, const std::string& mode)
	{
		return platform::ChildProcess::start(GNUBASE_IPCPEER, {connection.name(), mode});
	}

	platform::ChildProcess startHelper(const std::string& argument, const std::string& mode)
	{
		return platform::ChildProcess::start(GNUBASE_IPCPEER, {argument, mode});
	}

	// Waits up to ten seconds for a process to end.
	bool ends(uint32_t id)
	{
		const auto deadline = std::chrono::steady_clock::now() + 10s;
		while (platform::processExists(id) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(10ms);
		return !platform::processExists(id);
	}

	std::string readFile(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	}

	// Sends one block of `frames` frames whose samples count up from `start`, with `events` events.
	void sendBlock(ipc::Connection& engine, uint64_t sequence, uint32_t frames, float start, uint32_t events)
	{
		ipc::AudioBlock block;
		REQUIRE(engine.beginBlock(block));
		block.sequence = sequence;
		block.frames = frames;
		block.channels = 2;
		for (uint32_t i = 0; i < frames; ++i)
		{
			block.channel(0)[i] = start + static_cast<float>(i);
			block.channel(1)[i] = -(start + static_cast<float>(i));
		}
		for (uint32_t i = 0; i < events; ++i)
			REQUIRE(engine.sendEvent(1, 1, &i, sizeof(i)));
		block.eventCount = events;
		engine.sendBlock(block);
	}

	// Waits for the answer to a block sent with sendBlock and checks it.
	bool receiveEcho(ipc::Connection& engine, uint64_t sequence, uint32_t frames, float start, uint32_t events)
	{
		if (engine.waitForBlock(5s) != ipc::WaitResult::Ready)
			return false;
		ipc::AudioBlock block;
		if (engine.receiveBlock(block) != ipc::ReadStatus::Ok)
			return false;
		bool ok = block.sequence == sequence && block.frames == frames && block.channels == 2;
		for (uint32_t i = 0; ok && i < frames; ++i)
		{
			const float value = start + static_cast<float>(i);
			ok = block.channel(0)[i] == (value * 2.0f) + static_cast<float>(events) &&
				block.channel(1)[i] == (-value * 2.0f) + static_cast<float>(events);
		}
		engine.releaseBlock();
		return ok;
	}
}

TEST_CASE("audio blocks round-trip through a host process", "[integration]")
{
	ipc::Connection engine = ipc::Connection::create(config());
	platform::ChildProcess peer = startPeer(engine, "echo");
	REQUIRE(engine.waitForPeer(10s));
	CHECK(engine.peerProcessId() == peer.id());

	for (uint64_t sequence = 0; sequence < 500; ++sequence)
	{
		const auto frames = static_cast<uint32_t>(1 + (sequence % 256));
		const auto events = static_cast<uint32_t>(sequence % 5);
		const auto start = static_cast<float>(sequence);
		sendBlock(engine, sequence, frames, start, events);
		REQUIRE(receiveEcho(engine, sequence, frames, start, events));
	}

	// The control channel works alongside.
	const std::string ping = "ping";
	REQUIRE(engine.sendControl(1, 1, ping.data(), static_cast<uint32_t>(ping.size())));
	REQUIRE(engine.waitForControl(5s) == ipc::WaitResult::Ready);
	std::array<std::byte, 64> buffer{};
	ipc::MessageHeader header;
	REQUIRE(engine.receiveControl(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Ok);
	CHECK(header.type == 2);
	CHECK(std::string(reinterpret_cast<const char*>(buffer.data()), header.size) == ping);

	// Closing the connection ends the host.
	engine.close();
	CHECK(peer.wait(10s) == 0);
}

TEST_CASE("a killed host process misses its deadline", "[integration]")
{
	ipc::Connection engine = ipc::Connection::create(config());
	platform::ChildProcess peer = startPeer(engine, "echo");
	REQUIRE(engine.waitForPeer(10s));
	sendBlock(engine, 1, 64, 0.0f, 0);
	REQUIRE(receiveEcho(engine, 1, 64, 0.0f, 0));

	peer.kill();
	CHECK(!peer.running());
	sendBlock(engine, 2, 64, 0.0f, 0);
	const auto start = std::chrono::steady_clock::now();
	CHECK(engine.waitForBlock(50ms) == ipc::WaitResult::Timeout);
	CHECK(std::chrono::steady_clock::now() - start < 5s);
	// The peer never closed the connection: it crashed rather than hung, since its process is gone.
	CHECK(engine.peerState() == ipc::PeerState::Attached);
}

TEST_CASE("a hung host process misses its deadline but keeps running", "[integration]")
{
	ipc::Connection engine = ipc::Connection::create(config());
	platform::ChildProcess peer = startPeer(engine, "hang");
	REQUIRE(engine.waitForPeer(10s));
	sendBlock(engine, 1, 64, 0.0f, 0);
	CHECK(engine.waitForBlock(50ms) == ipc::WaitResult::Timeout);
	CHECK(peer.running());
	peer.kill();
	CHECK(!peer.running());
}

TEST_CASE("a host process that closes the connection is noticed at once", "[integration]")
{
	ipc::Connection engine = ipc::Connection::create(config());
	platform::ChildProcess peer = startPeer(engine, "close");
	REQUIRE(engine.waitForPeer(10s));
	CHECK(engine.waitForBlock(10s) == ipc::WaitResult::PeerClosed);
	CHECK(peer.wait(10s) == 0);
}

TEST_CASE("round-trip latency to a host process", "[integration][benchmark]")
{
	ipc::Connection engine = ipc::Connection::create(config());
	platform::ChildProcess peer = startPeer(engine, "echo");
	REQUIRE(engine.waitForPeer(10s));

	std::vector<double> microseconds;
	for (uint64_t sequence = 0; sequence < 2000; ++sequence)
	{
		const auto start = std::chrono::steady_clock::now();
		sendBlock(engine, sequence, 128, 0.0f, 1);
		REQUIRE(receiveEcho(engine, sequence, 128, 0.0f, 1));
		microseconds.push_back(
			std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
	}
	std::sort(microseconds.begin(), microseconds.end());
	const double median = microseconds[microseconds.size() / 2];
	const double p99 = microseconds[microseconds.size() * 99 / 100];
	std::printf("IPC round trip (128 frames, 2 channels): median %.1f us, 99%% %.1f us, max %.1f us\n", median, p99,
		microseconds.back());
	// Shared CI runners vary a lot; this only catches a transport that sleeps instead of waking up.
	CHECK(median < 5000.0);
	engine.close();
	CHECK(peer.wait(10s) == 0);
}

TEST_CASE("a host process gets no standard input or output, and its errors go to a file", "[integration]")
{
	// In headless mode stdout carries MCP, so a plugin's printf must not reach it.
	const test::TempDir folder;
	const std::filesystem::path errors = folder.path() / "host.log";
	platform::ChildProcess peer = platform::ChildProcess::start(GNUBASE_IPCPEER, {"-", "print"}, errors);
	CHECK(peer.wait(10s) == 0); // stdin is at its end at once
	const std::string text = readFile(errors);
	CHECK(text.find("to stderr") != std::string::npos);
	CHECK(text.find("to stdout") == std::string::npos);
}

TEST_CASE("a host process inherits no handles from the engine", "[integration]")
{
	const test::TempDir folder;
	const std::filesystem::path path = folder.path() / "engine.log";
#ifdef _WIN32
	SECURITY_ATTRIBUTES inheritable{};
	inheritable.nLength = sizeof(inheritable);
	inheritable.bInheritHandle = TRUE;
	HANDLE file =
		CreateFileW(path.c_str(), GENERIC_WRITE, 0, &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	REQUIRE(file != INVALID_HANDLE_VALUE);
	platform::ChildProcess peer = startHelper("-", "sleep");
	CloseHandle(file);
	// Opening the file without sharing fails while the host holds an inherited handle to it.
	HANDLE again = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	CHECK(again != INVALID_HANDLE_VALUE);
	if (again != INVALID_HANDLE_VALUE)
		CloseHandle(again);
	peer.kill();
#else
	const int file = ::open(path.c_str(), O_WRONLY | O_CREAT, 0600); // inheritable: no O_CLOEXEC
	REQUIRE(file >= 0);
	platform::ChildProcess peer = startHelper(std::to_string(file), "fd");
	CHECK(peer.wait(10s) == 0);
	::close(file);
#endif
}

TEST_CASE("killing a process waits only so long, and the process is still collected", "[integration]")
{
	platform::ChildProcess peer = startHelper("-", "sleep");
	const uint32_t id = peer.id();
	REQUIRE(platform::processExists(id));
	const auto start = std::chrono::steady_clock::now();
	peer.kill(0ms);
	CHECK(std::chrono::steady_clock::now() - start < 1s);
	CHECK(!peer.running());
	// A reaper collects it later, so no zombie is left (POSIX).
	CHECK(ends(id));
}

TEST_CASE("a host notices when the engine process is gone", "[integration]")
{
	// A process that has ended plays the engine.
	platform::ChildProcess gone = startHelper("-", "exit");
	REQUIRE(gone.wait(10s).has_value());
	const ipc::Connection engine = ipc::Connection::create(config());
	{
		const platform::SharedMemory memory = platform::SharedMemory::open(engine.name());
		static_cast<ipc::SegmentHeader*>(memory.data())->enginePid = gone.id();
	}
	ipc::Connection host = ipc::Connection::open(engine.name());
	CHECK(host.waitForBlock(20ms) == ipc::WaitResult::PeerClosed);
	CHECK(host.waitForControl(20ms) == ipc::WaitResult::PeerClosed);

	// With the engine alive, the same wait just times out.
	const ipc::Connection alive = ipc::Connection::create(config());
	ipc::Connection aliveHost = ipc::Connection::open(alive.name());
	CHECK(aliveHost.waitForBlock(20ms) == ipc::WaitResult::Timeout);
}

TEST_CASE("a host process ends when the engine process dies", "[integration]")
{
	// gnubase-ipcpeer plays the engine as well: it starts a host and writes the host's process id to a file.
	const test::TempDir folder;
	const std::filesystem::path file = folder.path() / "host.pid";
	platform::ChildProcess engine = startHelper(file.u8string(), "engine");
	uint32_t hostId = 0;
	const auto deadline = std::chrono::steady_clock::now() + 10s;
	while (hostId == 0 && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(10ms);
		std::ifstream(file) >> hostId;
	}
	REQUIRE(hostId != 0);
	REQUIRE(platform::processExists(hostId));

	engine.kill();
	CHECK(ends(hostId));
}
