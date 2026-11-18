// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// What the Windows crash handler (crashhandler.cpp) and gnubase-crashdump (crashdump.cpp), the program that writes its
// minidumps from outside the crashed process, share.
namespace gnubase::logging::detail
{
	// The program's file name. The crash handler starts it from the folder of the running program.
	constexpr const wchar_t* g_crashDumpProgram = L"gnubase-crashdump.exe";

	// gnubase-crashdump's arguments, in this order: the crash handler's process id, the values of its three event
	// handles (crashed, dumped, ready), the address of its CrashInfo, and the dump file.
	constexpr int g_crashDumpArguments = 6;

	// Filled by the crashed thread before it sets the crashed event. gnubase-crashdump reads it from the crashed
	// process's memory, so it holds no pointers into the helper's own: `exception` is an EXCEPTION_POINTERS* of the
	// crashed process, null for abort(). Its layout, and that of the exception records the dump reads through it, is
	// the architecture's, so the helper is built for the architecture of the programs next to it.
	struct CrashInfo
	{
		std::uint32_t threadId = 0;
		void* exception = nullptr;
	};
}
