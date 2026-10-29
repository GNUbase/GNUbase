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

	// Linux: has the kernel kill this process when its parent `parentId` ends (PR_SET_PDEATHSIG), if that is still
	// its parent. A plugin host calls it through Connection::open. The kernel goes by the thread that started the
	// process, not by the whole parent process, so start plugin hosts from a thread that lives as long as they do.
	// Does nothing elsewhere: on Windows a ChildProcess belongs to a job that ends it with GNUbase, and macOS has no
	// such signal, so plugin hosts check the engine's process when a wait times out (ProcessWatch).
	void endWithParent(uint32_t parentId) noexcept;

	// Watches a process this one did not start, such as a plugin host watching the engine, to notice that it is gone.
	// Holds the process on Windows, so its id cannot be reused meanwhile; on POSIX the parent is watched through
	// getppid, any other process by its id. gone() makes one system call that does not block.
	class ProcessWatch
	{
	public:
		ProcessWatch() = default;
		explicit ProcessWatch(uint32_t id);
		~ProcessWatch();
		ProcessWatch(ProcessWatch&& other) noexcept;
		ProcessWatch& operator=(ProcessWatch&& other) noexcept;
		ProcessWatch(const ProcessWatch&) = delete;
		ProcessWatch& operator=(const ProcessWatch&) = delete;

		// True once the process has ended. A default-constructed watch watches nothing and is never gone.
		bool gone() const noexcept;

	private:
		void release() noexcept;

		uint32_t m_id = 0;
		void* m_handle = nullptr;   // Windows
		bool m_parent = false;      // POSIX: the process is this one's parent
		bool m_goneAtStart = false; // it had ended before the watch began
	};

	// A process GNUbase started, such as a plugin host. Not for audio threads: every call is a system call that may
	// allocate. Destroying a ChildProcess that still runs kills it, so no process outlives its owner unnoticed.
	// The process starts with standard input and output on the null device, and inherits no other handles or file
	// descriptors than standard error. On Windows it has no console window, and it belongs to a job that ends it when
	// GNUbase ends, even if GNUbase crashes; on Linux and macOS see endWithParent.
	class ChildProcess
	{
	public:
		// How long kill() waits for a killed process before a reaper takes it over.
		static constexpr std::chrono::milliseconds g_killTimeout{1000};

		ChildProcess() = default;
		~ChildProcess();
		ChildProcess(ChildProcess&& other) noexcept;
		ChildProcess& operator=(ChildProcess&& other) noexcept;
		ChildProcess(const ChildProcess&) = delete;
		ChildProcess& operator=(const ChildProcess&) = delete;

		// Starts `program` with the arguments (UTF-8). Its standard error is appended to the file `errorOutput`, or
		// goes to GNUbase's own standard error if that is empty. Throws std::system_error if it cannot be started.
		static ChildProcess start(const std::filesystem::path& program, const std::vector<std::string>& arguments,
			const std::filesystem::path& errorOutput = {});

		// The process id, which is safe to kill: unlike an id a process reports about itself, nobody else can change
		// it.
		uint32_t id() const noexcept { return m_id; }

		// True until the process has exited. Does not wait.
		bool running();

		// Waits up to `timeout` for the process to exit and returns its exit code, or nothing if it still runs.
		// A process ended by a signal (POSIX) returns 128 plus the signal number, as shells report it.
		std::optional<int> wait(std::chrono::milliseconds timeout);

		// Ends the process at once, the way a crash would, and waits up to `timeout` until it is gone. A process stuck
		// in the kernel (driver I/O, Linux's D state) may take longer: a reaper then collects it in the background, so
		// no zombie is left, and the ChildProcess counts it as ended (exit code -1). Never blocks for longer.
		void kill(std::chrono::milliseconds timeout = g_killTimeout) noexcept;

	private:
		void release() noexcept;

		uint32_t m_id = 0;
		void* m_handle = nullptr; // the process handle on Windows
		std::optional<int> m_exitCode;
	};
}
