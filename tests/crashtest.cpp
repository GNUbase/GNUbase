// SPDX-License-Identifier: GPL-3.0-or-later
// Helper process for crash_tests.cpp: opens a log session in the given folder, then crashes.
#include "platform/log.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string_view>

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
		int* volatile pointer = nullptr;
		*pointer = 42; // NOLINT(clang-analyzer-core.NullDereference): crashing is the point
		return 0;
	}
	catch (const std::exception&)
	{
		return 3;
	}
}
