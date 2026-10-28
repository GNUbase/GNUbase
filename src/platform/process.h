// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gnubase::platform
{
	// The id of the calling process.
	uint32_t currentProcessId() noexcept;

	// True while a process with this id runs. Ids are reused once a process is gone, so a long-dead process may seem
	// to run again; plugin hosts use this to notice that the engine went away without closing its connections.
	bool processExists(uint32_t id) noexcept;

	// The running program's file, e.g. to find the programs installed next to it.
	std::filesystem::path currentExecutablePath();

	// A process GNUbase started, such as a plugin host. Not for audio threads: every call is a system call that may
	// allocate. Destroying a ChildProcess that still runs kills it, so no process outlives its owner unnoticed.
	class ChildProcess
	{
	public:
		ChildProcess() = default;
		~ChildProcess();
		ChildProcess(ChildProcess&& other) noexcept;
		ChildProcess& operator=(ChildProcess&& other) noexcept;
		ChildProcess(const ChildProcess&) = delete;
		ChildProcess& operator=(const ChildProcess&) = delete;

		// Starts `program` with the arguments (UTF-8). Throws std::system_error if it cannot be started.
		static ChildProcess start(const std::filesystem::path& program, const std::vector<std::string>& arguments);

		uint32_t id() const noexcept { return m_id; }

		// True until the process has exited. Does not wait.
		bool running();

		// Waits up to `timeout` for the process to exit and returns its exit code, or nothing if it still runs.
		// A process ended by a signal (POSIX) returns 128 plus the signal number, as shells report it.
		std::optional<int> wait(std::chrono::milliseconds timeout);

		// Ends the process at once, the way a crash would, and waits until it is gone.
		void kill() noexcept;

	private:
		void release() noexcept;

		uint32_t m_id = 0;
		void* m_handle = nullptr; // the process handle on Windows
		std::optional<int> m_exitCode;
	};
}
