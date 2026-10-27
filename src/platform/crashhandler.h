// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdio>
#include <filesystem>

// Used by logging::Session only.
namespace gnubase::logging::detail
{
	// Reports crashes to the log (may be null) and stderr: a backtrace on macOS and Linux, a note and a minidump
	// named like the log file with the extension .dmp on Windows.
	void installCrashHandler(std::FILE* log, const std::filesystem::path& logFile);

	// Restores the handlers that were installed before.
	void uninstallCrashHandler() noexcept;
}
