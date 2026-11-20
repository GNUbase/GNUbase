// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/crashhandler.h"

#include <array>
#include <csignal>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include "platform/crashdump.h"
#include "platform/process.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <system_error>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>
#include <io.h>
#else
#include <execinfo.h>
#include <iterator>
#include <unistd.h>
#endif

namespace gnubase::logging::detail
{
	namespace
	{
		int g_logDescriptor = -1;

		// Writes with plain system calls, which still work in a crashing process.
		void report(const char* text) noexcept
		{
			const size_t length = std::strlen(text);
#ifdef _WIN32
			if (g_logDescriptor >= 0)
				_write(g_logDescriptor, text, static_cast<unsigned>(length));
			_write(2, text, static_cast<unsigned>(length));
#else
			if (g_logDescriptor >= 0)
			{
				[[maybe_unused]] const ssize_t written = write(g_logDescriptor, text, length);
			}
			[[maybe_unused]] const ssize_t written = write(STDERR_FILENO, text, length);
#endif
		}

#ifdef _WIN32
		std::wstring g_dumpFile;
		LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;
		_crt_signal_t g_previousAbortHandler = SIG_DFL;

		// The crashed thread ends the process with TerminateProcess once the minidump is written, or after
		// g_dumpTimeoutMs (#1849). Ending it through the filter's return or abort's _exit runs the DLLs' detach code
		// after ending the other threads wherever they were, and the C runtime's clean-up there can block on a lock one
		// of them held. TerminateProcess runs no code of the process.
		//
		// The dump is written by gnubase-crashdump (crashdump.cpp), which the handler starts when it is installed and
		// which waits for a crash (#1883). MiniDumpWriteDump suspends every other thread of the process it dumps:
		// called in the crashed process, it can block on a lock one of them holds, such as the heap's, with the waiting
		// crashed thread among the suspended ones, and then no timeout ends the process. From another process it needs
		// none of the crashed process's locks. If the program can't be started, a thread of this process that waits
		// for a crash from the start writes the dump, and without that thread the crashed thread writes it itself;
		// both can still block that way.
		//
		// The first crash is the one the dump shows, and the process ends with its exit code: a thread that crashes
		// while another one's crash is handled waits for that to end the process.
		constexpr DWORD g_dumpTimeoutMs = 30000;
		// Well below the time a plugin host has to connect to the engine (HostManager::Options::launchTimeout), which
		// includes opening its log session.
		constexpr DWORD g_helperStartTimeoutMs = 2000;

		// g_state goes from idle to crashing with the first crash, or to uninstalled when the handler is removed. The
		// events and the helper stay until uninstallCrashHandler has taken it from idle to uninstalled, so a crash
		// never waits on a closed handle.
		constexpr int g_idle = 0;
		constexpr int g_crashing = 1;
		constexpr int g_uninstalled = 2;
		std::atomic<int> g_state{g_uninstalled};
		std::atomic<UINT> g_exitCode{0}; // the first crash's

		CrashInfo g_crashInfo;      // gnubase-crashdump reads it from this process's memory
		HANDLE g_crashed = nullptr; // set by the crashed thread, or by uninstallCrashHandler to end the dump thread
		HANDLE g_dumped = nullptr;
		std::optional<platform::ChildProcess> g_helper;
		HANDLE g_helperProcess = nullptr; // to notice that the helper is gone
		HANDLE g_dumpThread = nullptr;    // only without the helper

		void writeMinidump() noexcept
		{
			auto* const file = CreateFileW(
				g_dumpFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return;
			MINIDUMP_EXCEPTION_INFORMATION info{
				g_crashInfo.threadId, static_cast<EXCEPTION_POINTERS*>(g_crashInfo.exception), FALSE};
			MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal,
				g_crashInfo.exception != nullptr ? &info : nullptr, nullptr, nullptr);
			CloseHandle(file);
		}

		DWORD WINAPI dumpThread(void* /*unused*/)
		{
			WaitForSingleObject(g_crashed, INFINITE);
			if (g_state.load() == g_crashing)
				writeMinidump();
			SetEvent(g_dumped);
			return 0;
		}

		std::string handleValue(const void* handle)
		{
			return std::to_string(HandleToULong(handle));
		}

		// Starts gnubase-crashdump from the running program's folder and waits until it watches this process. False if
		// it can't be started; then nothing of it is left.
		bool startHelper() noexcept
		{
			HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (ready == nullptr)
				return false;
			try
			{
				const std::filesystem::path program =
					platform::currentExecutablePath().parent_path() / g_crashDumpProgram;
				std::error_code error;
				if (std::filesystem::is_regular_file(program, error))
					g_helper = platform::ChildProcess::start(program,
						{std::to_string(GetCurrentProcessId()), handleValue(g_crashed), handleValue(g_dumped),
							handleValue(ready), std::to_string(reinterpret_cast<std::uintptr_t>(&g_crashInfo)),
							std::filesystem::path(g_dumpFile).u8string()});
			}
			catch (const std::exception&)
			{
				g_helper.reset();
			}
			if (g_helper)
			{
				// The ChildProcess holds the helper, so its id is still its own.
				g_helperProcess = OpenProcess(SYNCHRONIZE, FALSE, g_helper->id());
				const std::array<HANDLE, 2> waits{ready, g_helperProcess};
				if (g_helperProcess == nullptr ||
					WaitForMultipleObjects(
						static_cast<DWORD>(waits.size()), waits.data(), FALSE, g_helperStartTimeoutMs) != WAIT_OBJECT_0)
				{
					if (g_helperProcess != nullptr)
						CloseHandle(g_helperProcess);
					g_helperProcess = nullptr;
					g_helper.reset(); // kills it
				}
			}
			CloseHandle(ready);
			return g_helper.has_value();
		}

		void stopHelper() noexcept
		{
			g_helper.reset(); // kills it
			if (g_helperProcess != nullptr)
				CloseHandle(g_helperProcess);
			g_helperProcess = nullptr;
		}

		[[noreturn]] void terminate(UINT exitCode) noexcept
		{
			TerminateProcess(GetCurrentProcess(), exitCode);
			std::_Exit(static_cast<int>(exitCode)); // not reached
		}

		// Another thread handles a crash and ends the process within g_dumpTimeoutMs, once the dump is written.
		// Ending it here would cut the dump short and end with this thread's exit code instead of that crash's.
		[[noreturn]] void awaitCrash() noexcept
		{
			Sleep(g_dumpTimeoutMs + 5000);
			terminate(g_exitCode.load());
		}

		[[noreturn]] void endCrashed(EXCEPTION_POINTERS* exception, UINT exitCode) noexcept
		{
			int state = g_idle;
			if (!g_state.compare_exchange_strong(state, g_crashing))
			{
				if (state == g_crashing)
					awaitCrash();
				terminate(exitCode); // the handler is being removed, and its events with it
			}
			g_exitCode.store(exitCode);
			g_crashInfo.threadId = GetCurrentThreadId();
			g_crashInfo.exception = exception;
			if (g_helperProcess != nullptr)
			{
				SetEvent(g_crashed);
				// The helper sets the event before it ends: if it is gone without, the dump is still to be written.
				const std::array<HANDLE, 2> waits{g_dumped, g_helperProcess};
				if (WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, g_dumpTimeoutMs) ==
					WAIT_OBJECT_0 + 1)
					writeMinidump();
			}
			else if (g_dumpThread != nullptr)
			{
				SetEvent(g_crashed);
				WaitForSingleObject(g_dumped, g_dumpTimeoutMs);
			}
			else
			{
				writeMinidump();
			}
			terminate(exitCode);
		}

		LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* exception)
		{
			std::array<char, 64> text{};
			std::snprintf(text.data(), text.size(), "\nCrash: exception 0x%08lX, writing a minidump\n",
				exception->ExceptionRecord->ExceptionCode);
			report(text.data());
			endCrashed(exception, exception->ExceptionRecord->ExceptionCode);
		}

		// abort() raises SIGABRT and never reaches the unhandled exception filter.
		void onAbort(int /*signal*/)
		{
			report("\nCrash: abort, writing a minidump\n");
			endCrashed(nullptr, 3); // abort's exit code
		}
#else
		constexpr std::array<int, 5> g_signals{SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
		std::array<struct sigaction, g_signals.size()> g_previousActions{};

		const char* signalName(int signal) noexcept
		{
			switch (signal)
			{
			case SIGSEGV:
				return "SIGSEGV";
			case SIGBUS:
				return "SIGBUS";
			case SIGILL:
				return "SIGILL";
			case SIGFPE:
				return "SIGFPE";
			case SIGABRT:
				return "SIGABRT";
			default:
				return "signal";
			}
		}

		void onSignal(int signal)
		{
			report("\nCrash: ");
			report(signalName(signal));
			report("\n");
			std::array<void*, 64> frames{};
			const int count = backtrace(frames.data(), static_cast<int>(frames.size()));
			if (g_logDescriptor >= 0)
				backtrace_symbols_fd(frames.data(), count, g_logDescriptor);
			backtrace_symbols_fd(frames.data(), count, STDERR_FILENO);
			// SA_RESETHAND restored the default action, which ends the process once this handler returns.
			std::raise(signal);
		}
#endif
	}

	void installCrashHandler(std::FILE* log, const std::filesystem::path& logFile)
	{
#ifdef _WIN32
		g_logDescriptor = log != nullptr ? _fileno(log) : -1;
		std::filesystem::path dumpFile = logFile;
		g_dumpFile = dumpFile.replace_extension(".dmp").wstring();
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // no message box, no system crash reporter
		if (g_state.load() == g_uninstalled)
		{
			g_crashInfo = CrashInfo{};
			g_crashed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			g_dumped = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (g_crashed != nullptr && g_dumped != nullptr && !startHelper())
				g_dumpThread = CreateThread(nullptr, 0, &dumpThread, nullptr, 0, nullptr);
			g_state.store(g_idle);
		}
		g_previousFilter = SetUnhandledExceptionFilter(&onUnhandledException);
		g_previousAbortHandler = std::signal(SIGABRT, &onAbort);
#else
		static_cast<void>(logFile);
		g_logDescriptor = log != nullptr ? fileno(log) : -1;
		std::array<void*, 1> frame{};
		backtrace(frame.data(), 1); // the first call loads the unwinder, which may allocate
		struct sigaction action{};
		action.sa_handler = &onSignal;
		sigemptyset(&action.sa_mask);
		action.sa_flags = SA_RESETHAND;
		for (size_t i = 0; i < g_signals.size(); ++i)
			sigaction(g_signals[i], &action, &g_previousActions[i]);
#endif
	}

	void uninstallCrashHandler() noexcept
	{
#ifdef _WIN32
		SetUnhandledExceptionFilter(g_previousFilter);
		std::signal(SIGABRT, g_previousAbortHandler);
		// A crash that is being handled ends the process, and needs the events and the helper until then. A filter
		// that was entered before it was removed and finds the handler uninstalled ends the process without a dump.
		int state = g_idle;
		if (!g_state.compare_exchange_strong(state, g_uninstalled) && state == g_crashing)
			awaitCrash();
		stopHelper();
		if (g_dumpThread != nullptr)
		{
			SetEvent(g_crashed); // the thread finds the handler uninstalled and ends without writing a dump
			WaitForSingleObject(g_dumpThread, INFINITE);
			CloseHandle(g_dumpThread);
			g_dumpThread = nullptr;
		}
		for (HANDLE* event : {&g_crashed, &g_dumped})
			if (*event != nullptr)
			{
				CloseHandle(*event);
				*event = nullptr;
			}
#else
		for (size_t i = 0; i < g_signals.size(); ++i)
			sigaction(g_signals[i], &g_previousActions[i], nullptr);
#endif
		g_logDescriptor = -1;
	}
}
