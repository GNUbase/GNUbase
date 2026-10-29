// SPDX-License-Identifier: GPL-3.0-or-later
// Helper process for ipc_process_tests.cpp: plays a plugin host on the other end of an IPC connection.
//   gnubase-ipcpeer <name> echo    answers every block with its input times two plus the number of its events,
//                                  answers control message 1 (ping) with 2 (pong), and exits when the engine closes
//                                  or its process is gone
//   gnubase-ipcpeer <name> hang    attaches, then never answers
//   gnubase-ipcpeer <name> close   attaches and closes at once
// It also plays other processes for the process tests:
//   gnubase-ipcpeer <file> engine  plays the engine: starts an echo host, writes its process id to the file, then
//                                  waits to be killed
//   gnubase-ipcpeer - print        writes to stdout and stderr; exits 0 if stdin is at its end
//   gnubase-ipcpeer - sleep        sleeps for a minute
//   gnubase-ipcpeer - exit         exits at once
//   gnubase-ipcpeer <fd> fd        exits 0 if file descriptor <fd> is closed (POSIX)
#include "ipc/connection.h"
#include "platform/process.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#endif

using namespace gnubase;
using namespace std::chrono_literals;

namespace
{
	constexpr uint16_t g_ping = 1;
	constexpr uint16_t g_pong = 2;

	// Answers the control messages that are waiting.
	void answerControl(ipc::Connection& connection)
	{
		std::array<std::byte, 256> buffer{};
		ipc::MessageHeader header;
		while (connection.receiveControl(header, buffer.data(), buffer.size()) == ipc::ReadStatus::Ok)
		{
			if (header.type == g_ping)
				connection.sendControl(g_pong, 1, buffer.data(), header.size);
		}
	}

	int echo(ipc::Connection& connection)
	{
		for (;;)
		{
			const ipc::WaitResult result = connection.waitForBlock(10ms);
			answerControl(connection);
			if (result == ipc::WaitResult::PeerClosed)
				return 0;
			if (result == ipc::WaitResult::Timeout)
				continue;
			ipc::AudioBlock input;
			while (connection.receiveBlock(input) == ipc::ReadStatus::Ok)
			{
				std::array<std::byte, 64> event{};
				ipc::MessageHeader header;
				for (uint32_t i = 0; i < input.eventCount; ++i)
				{
					if (connection.receiveEvent(header, event.data(), event.size()) != ipc::ReadStatus::Ok)
						return 4;
				}
				ipc::AudioBlock output;
				if (!connection.beginBlock(output))
					return 5;
				output.sequence = input.sequence;
				output.position = input.position;
				output.frames = input.frames;
				output.channels = input.channels;
				for (uint32_t channel = 0; channel < input.channels; ++channel)
				{
					for (uint32_t frame = 0; frame < input.frames; ++frame)
						output.channel(channel)[frame] =
							(input.channel(channel)[frame] * 2.0f) + static_cast<float>(input.eventCount);
				}
				connection.releaseBlock();
				connection.sendBlock(output);
			}
		}
	}

	int engine(const std::filesystem::path& program, const std::filesystem::path& file)
	{
		ipc::Connection connection = ipc::Connection::create(ipc::Config{});
		const platform::ChildProcess host = platform::ChildProcess::start(program, {connection.name(), "echo"});
		if (!connection.waitForPeer(10s))
			return 6;
		std::filesystem::path partial = file;
		partial += ".partial";
		std::ofstream(partial) << host.id() << '\n';
		std::filesystem::rename(partial, file);
		for (;;)
			std::this_thread::sleep_for(1s);
	}

	int fileDescriptorClosed([[maybe_unused]] const char* number)
	{
#ifdef _WIN32
		return 2;
#else
		return fcntl(std::stoi(number), F_GETFD) == -1 ? 0 : 1;
#endif
	}
}

int main(int argc, char** argv)
{
	if (argc != 3)
		return 2;
	try
	{
		const std::string_view mode = argv[2];
		if (mode == "engine")
			return engine(std::filesystem::u8path(argv[0]), std::filesystem::u8path(argv[1]));
		if (mode == "print")
		{
			std::fputs("to stdout\n", stdout);
			std::fputs("to stderr\n", stderr);
			return std::getchar() == EOF ? 0 : 1;
		}
		if (mode == "sleep")
		{
			std::this_thread::sleep_for(60s);
			return 0;
		}
		if (mode == "fd")
			return fileDescriptorClosed(argv[1]);
		if (mode == "exit")
			return 0;

		ipc::Connection connection = ipc::Connection::open(argv[1]);
		if (mode == "echo")
			return echo(connection);
		if (mode == "hang")
		{
			for (;;)
				std::this_thread::sleep_for(1s);
		}
		if (mode == "close")
			return 0;
		return 2;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "gnubase-ipcpeer: %s\n", error.what());
		return 3;
	}
}
