// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
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

	// The folder the OS keeps for the user, for when the environment doesn't name it: the roaming application data
	// on Windows (SHGetKnownFolderPath), the home folder from the user database elsewhere (getpwuid_r).
	using UserFolder = std::function<std::optional<std::filesystem::path>()>;

	// This user's folder, as the OS has it; nullopt if it has none.
	std::optional<std::filesystem::path> osUserFolder();

	// Overrides the settings folder, for tests and portable installs.
	inline constexpr const char* g_settingsDirectoryVariable = "GNUBASE_SETTINGS_DIR";

	// The per-user settings folder: GNUBASE_SETTINGS_DIR if it is set, otherwise %APPDATA%\GNUbase on Windows,
	// ~/Library/Application Support/GNUbase on macOS, and $XDG_CONFIG_HOME/gnubase or ~/.config/gnubase elsewhere.
	// Without those variables it asks the OS for the user's folder (userFolder: the roaming application data on
	// Windows, the home folder elsewhere). Empty if there is none: never a folder that other users share.
	std::filesystem::path settingsDirectory(Os os, const Environment& environment, const UserFolder& userFolder);

	// The same for this OS and process.
	std::filesystem::path settingsDirectory();

	// Replaces the file's contents, or creates the file and its folder, so that it has either the old contents or the
	// new ones even if the process dies or the disk fills up meanwhile: writes a temporary file next to it, flushes it
	// to the disk, then renames it over the file. A symbolic link stays one: the file it points to is replaced. Throws
	// std::system_error if that fails, leaving the file as it was. Not for audio threads.
	void writeAtomically(const std::filesystem::path& file, const std::string& contents);

	// Removes the temporary files that writeAtomically left next to the file when a process died while writing it:
	// <file>.<16 hex digits>.tmp. Only while no other process writes the file, e.g. under its FileLock.
	void removeStaleTemporaries(const std::filesystem::path& file);

	// A file's contents, or nullopt if it can't be read. Not for audio threads.
	std::optional<std::string> readFile(const std::filesystem::path& file);

	// An exclusive lock between processes, and between the threads of one, held while the object lives: an OS lock on
	// the lock file, created if needed and kept afterwards. The OS releases it if the process dies.
	class FileLock
	{
	public:
		// Waits up to the timeout for the lock; throws std::system_error if it can't be taken by then.
		FileLock(const std::filesystem::path& file, std::chrono::milliseconds timeout);
		~FileLock();
		FileLock(const FileLock&) = delete;
		FileLock& operator=(const FileLock&) = delete;
		FileLock(FileLock&&) = delete;
		FileLock& operator=(FileLock&&) = delete;

	private:
		intptr_t m_handle = -1; // a HANDLE on Windows, a file descriptor elsewhere
	};
}
