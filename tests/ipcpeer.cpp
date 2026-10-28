// SPDX-License-Identifier: GPL-3.0-or-later
// Helper process for ipc_process_tests.cpp: plays a plugin host on the other end of an IPC connection.
//   gnubase-ipcpeer <name> echo   answers every block with its input times two plus the number of its events,
//                                 answers control message 1 (ping) with 2 (pong), and exits when the engine closes
//   gnubase-ipcpeer <name> hang   attaches, then never answers
//   gnubase-ipcpeer <name> close  attaches and closes at once
#include "ipc/connection.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string_view>
#include <thread>

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
}

int main(int argc, char** argv)
{
	if (argc != 3)
		return 2;
	try
	{
		ipc::Connection connection = ipc::Connection::open(argv[1]);
		const std::string_view mode = argv[2];
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
