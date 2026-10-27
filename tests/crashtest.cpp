// SPDX-License-Identifier: GPL-3.0-or-later
// Helper process for crash_tests.cpp: opens a log session in the given folder, then crashes.
#include "platform/log.h"

#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string_view>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int main(int argc, char** argv)
{
	if (argc != 3)
		return 2;
	try
	{
		const gnubase::logging::Session session(std::filesystem::path(argv[1]) / "crash.log");
		gnubase::logging::info("about to crash");
		if (std::string_view(argv[2]) == "abort")
			std::abort();
		// A real access violation, not a null write, which optimizers may remove.
#ifdef _WIN32
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
