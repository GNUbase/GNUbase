// SPDX-License-Identifier: GPL-3.0-or-later
// Helper process for crash_tests.cpp: opens a log session in the given folder, then crashes the way it is told (or,
// with "helper", checks that the session starts and ends its gnubase-crashdump).
#include "platform/log.h"

#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string_view>

#ifdef _WIN32
#include <atomic>
#include <optional>
#include <thread>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// tlhelp32.h needs windows.h first.
#include <tlhelp32.h>
#endif

namespace
{
#ifdef _WIN32
	// Crashes another thread with EXCEPTION_ILLEGAL_INSTRUCTION at the moment this one crashes with an access
	// violation, so that one of them crashes while the other one's crash is being handled.
	void crashTwice()
	{
		std::atomic<bool> ready{false};
		std::atomic<bool> go{false};
		std::thread other(
			[&ready, &go]
			{
				ready = true;
				while (!go.load())
				{
				}
				RaiseException(EXCEPTION_ILLEGAL_INSTRUCTION, 0, 0, nullptr);
			});
		while (!ready.load())
		{
		}
		go = true;
		RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
		other.join(); // not reached
	}

	// Crashes while another thread holds the heap's lock. A minidump written in this process suspends that thread,
	// and if MiniDumpWriteDump then needs the heap, it waits for the lock forever.
	void crashWithHeapLocked()
	{
		std::atomic<bool> locked{false};
		std::thread holder(
			[&locked]
			{
				HeapLock(GetProcessHeap());
				locked = true;
				Sleep(INFINITE);
			});
		while (!locked.load())
		{
		}
		RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
		holder.join(); // not reached
	}

	// The ids of the gnubase-crashdump processes this process started.
	std::vector<DWORD> crashDumpHelpers()
	{
		std::vector<DWORD> helpers;
		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE)
			return helpers;
		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE; more = Process32NextW(snapshot, &entry))
			if (entry.th32ParentProcessID == GetCurrentProcessId() &&
				std::wstring_view(entry.szExeFile) == L"gnubase-crashdump.exe")
				helpers.push_back(entry.th32ProcessID);
		CloseHandle(snapshot);
		return helpers;
	}

	// 0 if the log session starts one gnubase-crashdump and ends it with the session.
	int startAndEndHelper(const std::filesystem::path& log)
	{
		{
			const gnubase::logging::Session session(log);
			if (crashDumpHelpers().size() != 1)
				return 5;
		}
		return crashDumpHelpers().empty() ? 0 : 6;
	}

	// Ends the session's gnubase-crashdump, then crashes.
	void crashWithoutHelper()
	{
		for (const DWORD id : crashDumpHelpers())
			if (HANDLE helper = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, id); helper != nullptr)
			{
				TerminateProcess(helper, 1);
				WaitForSingleObject(helper, INFINITE);
				CloseHandle(helper);
			}
		RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
	}

	// Crashes another thread while this one ends the log session, which removes the crash handler.
	int crashWhileEnding(const std::filesystem::path& log)
	{
		std::optional<gnubase::logging::Session> session(std::in_place, log);
		std::atomic<bool> ready{false};
		std::atomic<bool> go{false};
		std::thread other(
			[&ready, &go]
			{
				ready = true;
				while (!go.load())
				{
				}
				RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
			});
		while (!ready.load())
		{
		}
		go = true;
		session.reset();
		other.join(); // the crash ends the process
		return 4;
	}
#endif
}

int main(int argc, char** argv)
{
	if (argc != 3)
		return 2;
#ifdef _WIN32
	// A crash that no handler takes must not wait for an error dialog.
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
	try
	{
		const std::filesystem::path log = std::filesystem::path(argv[1]) / "crash.log";
		const std::string_view how(argv[2]);
#ifdef _WIN32
		if (how == "ending")
			return crashWhileEnding(log);
		if (how == "helper")
			return startAndEndHelper(log);
#endif
		const gnubase::logging::Session session(log);
		gnubase::logging::info("about to crash");
		if (how == "abort")
			std::abort();
#ifdef _WIN32
		if (how == "twice")
			crashTwice();
		if (how == "heaplocked")
			crashWithHeapLocked();
		if (how == "helperkilled")
			crashWithoutHelper();
		// A real access violation, not a null write, which optimizers may remove.
		RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#else
		std::raise(SIGSEGV);
#endif
		return 0;
	}
	catch (const std::exception&)
	{
		return 3;
	}
}
