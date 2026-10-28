// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace gnubase::files
{
	enum class Os : uint8_t
	{
		Windows,
		MacOs,
		Linux, // and the other Unix-like systems
	};

	// The OS this build runs on.
	Os currentOs();

	// Looks up an environment variable; nullopt if it isn't set.
	using Environment = std::function<std::optional<std::filesystem::path>(const char* name)>;

	// This process's environment. On Windows it reads the variables as UTF-16, so any path comes through.
	std::optional<std::filesystem::path> processEnvironment(const char* name);

	// Overrides the settings folder, for tests and portable installs.
	inline constexpr const char* g_settingsDirectoryVariable = "GNUBASE_SETTINGS_DIR";

	// The per-user settings folder: GNUBASE_SETTINGS_DIR if it is set, otherwise %APPDATA%\GNUbase on Windows,
	// ~/Library/Application Support/GNUbase on macOS, and $XDG_CONFIG_HOME/gnubase or ~/.config/gnubase elsewhere.
	// Falls back to a folder in the temporary directory if the environment has no home.
	std::filesystem::path settingsDirectory(Os os, const Environment& environment);

	// The same for this OS and process.
	std::filesystem::path settingsDirectory();

	// Replaces the file's contents, or creates the file and its folder, so that it has either the old contents or the
	// new ones even if the process dies or the disk fills up meanwhile: writes a temporary file next to it, flushes it
	// to the disk, then renames it over the file. Throws std::system_error if that fails, leaving the file as it was.
	// Not for audio threads.
	void writeAtomically(const std::filesystem::path& file, const std::string& contents);

	// A file's contents, or nullopt if it can't be read. Not for audio threads.
	std::optional<std::string> readFile(const std::filesystem::path& file);
}
