// SPDX-License-Identifier: GPL-3.0-or-later
// gnubase-crashdump: writes the minidump of a GNUbase process that crashed, from outside it (Windows, #1883).
//
// MiniDumpWriteDump suspends every other thread of the process it dumps. Called inside the crashed process, it can
// block on a lock one of them holds, such as the heap's, and then nothing ends the process. From another process it
// needs none of the crashed process's locks. The crash handler starts this program when it is installed; it waits
// for a crash, writes the dump, says so and ends. It also ends when the process it watches ends without a crash.
#include "platform/crashdump.h"

#include <array>
#include <cstdlib>
#include <cwchar>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>

namespace
{
	using gnubase::logging::detail::CrashInfo;

	constexpr int g_dumpAttempts = 3;

	// Closes a handle when it goes out of scope.
	class Handle
	{
	public:
		explicit Handle(HANDLE handle = nullptr) noexcept : m_handle(handle) {}
		~Handle()
		{
			if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE)
				CloseHandle(m_handle);
		}
		Handle(const Handle&) = delete;
		Handle& operator=(const Handle&) = delete;
		Handle(Handle&&) = delete;
		Handle& operator=(Handle&&) = delete;

		HANDLE get() const noexcept { return m_handle; }

	private:
		HANDLE m_handle;
	};

	unsigned long long number(const wchar_t* text) noexcept
	{
		return std::wcstoull(text, nullptr, 10);
	}

	// A handle of the crashed process, given by its value there, as one of this process.
	HANDLE duplicate(HANDLE process, const wchar_t* value) noexcept
	{
		HANDLE own = nullptr;
		if (DuplicateHandle(process, ULongToHandle(static_cast<unsigned long>(number(value))), GetCurrentProcess(),
				&own, 0, FALSE, DUPLICATE_SAME_ACCESS) == FALSE)
			return nullptr;
		return own;
	}

	void writeDump(HANDLE process, DWORD processId, const void* infoAddress, const wchar_t* dumpFile) noexcept
	{
		CrashInfo info;
		SIZE_T read = 0;
		if (ReadProcessMemory(process, infoAddress, &info, sizeof(info), &read) == FALSE || read != sizeof(info))
			info = CrashInfo{};
		// ClientPointers: the exception pointers are the crashed process's.
		MINIDUMP_EXCEPTION_INFORMATION exception{info.threadId, static_cast<EXCEPTION_POINTERS*>(info.exception), TRUE};
		// MiniDumpWriteDump of another process fails now and then, for instance with ERROR_PARTIAL_COPY while one of
		// its threads starts or ends, and leaves an empty file: write it again from the start.
		for (int attempt = 0; attempt < g_dumpAttempts; ++attempt)
		{
			const Handle file(
				CreateFileW(dumpFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
			if (file.get() != INVALID_HANDLE_VALUE &&
				MiniDumpWriteDump(process, processId, file.get(), MiniDumpNormal,
					info.exception != nullptr ? &exception : nullptr, nullptr, nullptr) != FALSE)
				return;
		}
	}
}

// NOLINTNEXTLINE(misc-use-internal-linkage): wmain is the program's entry point, like main
int wmain(int argc, wchar_t** argv)
{
	if (argc != gnubase::logging::detail::g_crashDumpArguments + 1)
		return 2;
	const auto processId = static_cast<DWORD>(number(argv[1]));
	// The crash handler waits for the ready event, so its process still runs and its id is still its own.
	const Handle process(
		OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, processId));
	if (process.get() == nullptr)
		return 3;
	const Handle crashed(duplicate(process.get(), argv[2]));
	const Handle dumped(duplicate(process.get(), argv[3]));
	const Handle ready(duplicate(process.get(), argv[4]));
	if (crashed.get() == nullptr || dumped.get() == nullptr || ready.get() == nullptr)
		return 4;
	// NOLINTNEXTLINE(performance-no-int-to-ptr): an address in the crashed process, which this one only reads from
	const auto* const infoAddress = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(number(argv[5])));
	SetEvent(ready.get());

	const std::array<HANDLE, 2> waits{crashed.get(), process.get()};
	if (WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE) != WAIT_OBJECT_0)
		return 0; // the process ended without a crash
	writeDump(process.get(), processId, infoAddress, argv[6]);
	SetEvent(dumped.get());
	return 0;
}
