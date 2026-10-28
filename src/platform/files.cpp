// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/files.h"

#include "platform/rt/realtime.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gnubase::files
{
	namespace fs = std::filesystem;

	namespace
	{
		// The XDG base directory specification only takes absolute paths. Checked by hand, so that tests of the
		// Linux rules also run on Windows, where "/home" has no drive and so isn't absolute.
		bool isPosixAbsolute(const fs::path& path)
		{
			return !path.native().empty() && path.native().front() == '/';
		}

		struct FileCloser
		{
			void operator()(std::FILE* file) const { std::fclose(file); }
		};

		std::system_error writeError(const fs::path& file)
		{
			return {errno, std::generic_category(), "cannot write " + file.u8string()};
		}

		// Writes the file and flushes it to the disk.
		void writeDurably(const fs::path& file, const std::string& contents)
		{
#ifdef _WIN32
			std::unique_ptr<std::FILE, FileCloser> out(_wfopen(file.c_str(), L"wb"));
#else
			std::unique_ptr<std::FILE, FileCloser> out(std::fopen(file.c_str(), "wb"));
#endif
			if (!out)
				throw writeError(file);
			if (std::fwrite(contents.data(), 1, contents.size(), out.get()) != contents.size() ||
				std::fflush(out.get()) != 0)
				throw writeError(file);
#ifdef _WIN32
			if (_commit(_fileno(out.get())) != 0)
				throw writeError(file);
#else
			if (fsync(fileno(out.get())) != 0)
				throw writeError(file);
#endif
			if (std::fclose(out.release()) != 0)
				throw writeError(file);
		}

		// Makes a rename in the folder durable. Windows does that with the rename itself.
		void syncDirectory([[maybe_unused]] const fs::path& directory)
		{
#ifndef _WIN32
			const int descriptor = open(directory.empty() ? "." : directory.c_str(), O_RDONLY);
			if (descriptor >= 0)
			{
				fsync(descriptor);
				close(descriptor);
			}
#endif
		}
	}

	Os currentOs()
	{
#ifdef _WIN32
		return Os::Windows;
#elif defined(__APPLE__)
		return Os::MacOs;
#else
		return Os::Linux;
#endif
	}

	std::optional<fs::path> processEnvironment(const char* name)
	{
#ifdef _WIN32
		const std::wstring wideName(name, name + std::char_traits<char>::length(name)); // names are ASCII
		if (const wchar_t* value = _wgetenv(wideName.c_str()); value != nullptr)
			return fs::path(value);
#else
		if (const char* value = std::getenv(name); value != nullptr)
			return fs::path(value);
#endif
		return std::nullopt;
	}

	fs::path settingsDirectory(Os os, const Environment& environment)
	{
		const auto variable = [&environment](const char* name) -> std::optional<fs::path>
		{
			std::optional<fs::path> value = environment(name);
			if (value && value->empty())
				return std::nullopt; // set but empty counts as not set
			return value;
		};
		if (std::optional<fs::path> custom = variable(g_settingsDirectoryVariable))
			return *custom;
		switch (os)
		{
		case Os::Windows:
			// The roaming folder: settings follow the user to other computers of the domain.
			if (const std::optional<fs::path> appData = variable("APPDATA"))
				return *appData / "GNUbase";
			if (const std::optional<fs::path> profile = variable("USERPROFILE"))
				return *profile / "AppData" / "Roaming" / "GNUbase";
			break;
		case Os::MacOs:
			// Not ~/Library/Preferences: that folder belongs to the system's preferences service (property lists
			// that cfprefsd caches), while files that an application writes itself go to Application Support.
			if (const std::optional<fs::path> home = variable("HOME"))
				return *home / "Library" / "Application Support" / "GNUbase";
			break;
		case Os::Linux:
			if (const std::optional<fs::path> config = variable("XDG_CONFIG_HOME"); config && isPosixAbsolute(*config))
				return *config / "gnubase";
			if (const std::optional<fs::path> home = variable("HOME"))
				return *home / ".config" / "gnubase";
			break;
		}
		return fs::temp_directory_path() / "gnubase" / "settings";
	}

	fs::path settingsDirectory()
	{
		return settingsDirectory(currentOs(), processEnvironment);
	}

	void writeAtomically(const fs::path& file, const std::string& contents)
	{
		rt::assertNotRealtime("files::writeAtomically");
		const fs::path directory = file.parent_path();
		if (!directory.empty())
			fs::create_directories(directory);

		// A name of its own, so that two processes writing the same file don't write into each other's copy.
		std::random_device random;
		const uint64_t suffix = (uint64_t{random()} << 32U) ^ random();
		char hex[17] = {};
		std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(suffix));
		fs::path temporary = file;
		temporary += std::string(".") + hex + ".tmp";
		try
		{
			writeDurably(temporary, contents);
			fs::rename(temporary, file); // replaces the file in one step
		}
		catch (...)
		{
			std::error_code ignored;
			fs::remove(temporary, ignored);
			throw;
		}
		syncDirectory(directory);
	}

	std::optional<std::string> readFile(const fs::path& file)
	{
		rt::assertNotRealtime("files::readFile");
		std::error_code error;
		if (!fs::is_regular_file(file, error))
			return std::nullopt;
		std::ifstream in(file, std::ios::binary);
		if (!in)
			return std::nullopt;
		std::string contents{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
		if (in.bad())
			return std::nullopt;
		return contents;
	}
}
