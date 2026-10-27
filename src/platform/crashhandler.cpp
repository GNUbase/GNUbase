// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/crashhandler.h"

#include <array>
#include <csignal>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>
#include <io.h>
#include <string>
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

		void writeMinidump(EXCEPTION_POINTERS* exception) noexcept
		{
			auto* const file = CreateFileW(
				g_dumpFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return;
			MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), exception, FALSE};
			MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal,
				exception != nullptr ? &info : nullptr, nullptr, nullptr);
			CloseHandle(file);
		}

		LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* exception)
		{
			std::array<char, 64> text{};
			std::snprintf(text.data(), text.size(), "\nCrash: exception 0x%08lX, minidump written\n",
				exception->ExceptionRecord->ExceptionCode);
			report(text.data());
			writeMinidump(exception);
			return EXCEPTION_EXECUTE_HANDLER;
		}

		// abort() raises SIGABRT and never reaches the unhandled exception filter.
		void onAbort(int /*signal*/)
		{
			report("\nCrash: abort, minidump written\n");
			writeMinidump(nullptr);
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
#else
		for (size_t i = 0; i < g_signals.size(); ++i)
			sigaction(g_signals[i], &g_previousActions[i], nullptr);
#endif
		g_logDescriptor = -1;
	}
}
