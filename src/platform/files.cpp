// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/files.h"

#include "platform/rt/realtime.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// shlobj.h needs windows.h first.
#include <io.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace gnubase::files
{
	namespace fs = std::filesystem;

	namespace
	{
		// writeAtomically's temporary files are <file>.<g_temporaryDigits hex digits><g_temporaryExtension>.
		constexpr size_t g_temporaryDigits = 16;
		constexpr std::string_view g_temporaryExtension = ".tmp";

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
			std::FILE* opened = nullptr;
			if (_wfopen_s(&opened, file.c_str(), L"wb") != 0)
				opened = nullptr;
			std::unique_ptr<std::FILE, FileCloser> out(opened);
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
			const int descriptor = open(directory.empty() ? "." : directory.c_str(), O_RDONLY | O_CLOEXEC);
			if (descriptor >= 0)
			{
				fsync(descriptor);
				close(descriptor);
			}
#endif
		}

		// The file a symbolic link points to, through any number of links; the path itself if it is no link, or a
		// link to nothing.
		fs::path resolveLinks(const fs::path& file)
		{
			std::error_code error;
			if (!fs::is_symlink(fs::symlink_status(file, error)))
				return file;
			const fs::path target = fs::canonical(file, error);
			return error ? file : target;
		}

		bool isTemporaryOf(const std::string& name, const std::string& file)
		{
			const size_t start = file.size() + 1; // after "<file>."
			if (name.size() != start + g_temporaryDigits + g_temporaryExtension.size() ||
				name.rfind(file + ".", 0) != 0 ||
				name.compare(start + g_temporaryDigits, std::string::npos, g_temporaryExtension) != 0)
				return false;
			return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(start),
				name.begin() + static_cast<std::ptrdiff_t>(start + g_temporaryDigits),
				[](char digit) { return std::isxdigit(static_cast<unsigned char>(digit)) != 0; });
		}

		[[noreturn]] void throwLockError(int error, const std::error_category& category, const fs::path& file)
		{
			throw std::system_error(error, category, "cannot lock " + file.u8string());
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
		wchar_t* value = nullptr;
		std::size_t length = 0;
		const errno_t error = _wdupenv_s(&value, &length, wideName.c_str());
		const std::unique_ptr<wchar_t, decltype(&std::free)> owned(value, &std::free);
		if (error == 0 && value != nullptr)
			return fs::path(value);
#else
		if (const char* value = std::getenv(name); value != nullptr)
			return fs::path(value);
#endif
		return std::nullopt;
	}

	std::optional<fs::path> osUserFolder()
	{
#ifdef _WIN32
		PWSTR folder = nullptr;
		std::optional<fs::path> result;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &folder)) &&
			folder != nullptr && *folder != L'\0')
			result = fs::path(folder);
		CoTaskMemFree(folder);
		return result;
#else
		const long suggested = sysconf(_SC_GETPW_R_SIZE_MAX);
		std::vector<char> buffer(suggested > 0 ? static_cast<size_t>(suggested) : 16384);
		for (;;)
		{
			passwd entry{};
			passwd* found = nullptr;
			const int error = getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &found);
			if (error == ERANGE && buffer.size() < std::size_t{1024} * 1024)
			{
				buffer.resize(buffer.size() * 2);
				continue;
			}
			if (error != 0 || found == nullptr || found->pw_dir == nullptr || *found->pw_dir == '\0')
				return std::nullopt;
			return fs::path(found->pw_dir);
		}
#endif
	}

	fs::path settingsDirectory(Os os, const Environment& environment, const UserFolder& userFolder)
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
		// On macOS and Linux, the folder in the user's home.
		const auto inHome = [os](const fs::path& home)
		{
			// Not ~/Library/Preferences on macOS: that folder belongs to the system's preferences service (property
			// lists that cfprefsd caches), while files that an application writes itself go to Application Support.
			return os == Os::MacOs ? home / "Library" / "Application Support" / "GNUbase"
								   : home / ".config" / "gnubase";
		};
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
			if (const std::optional<fs::path> home = variable("HOME"))
				return inHome(*home);
			break;
		case Os::Linux:
			if (const std::optional<fs::path> config = variable("XDG_CONFIG_HOME"); config && isPosixAbsolute(*config))
				return *config / "gnubase";
			if (const std::optional<fs::path> home = variable("HOME"))
				return inHome(*home);
			break;
		}
		// No variable names the folder: ask the OS. Never a shared folder such as the temporary one, where other
		// users could read or plant the settings.
		const std::optional<fs::path> folder = userFolder ? userFolder() : std::nullopt;
		if (!folder || folder->empty())
			return {};
		return os == Os::Windows ? *folder / "GNUbase" : inHome(*folder);
	}

	fs::path settingsDirectory()
	{
		return settingsDirectory(currentOs(), processEnvironment, osUserFolder);
	}

	void writeAtomically(const fs::path& file, const std::string& contents)
	{
		rt::assertNotRealtime("files::writeAtomically");
		// A link to the file, as dotfile managers make them, stays a link: the file it points to is replaced, and the
		// temporary file is written next to that one, on the same file system, so that the rename stays one step.
		const fs::path target = resolveLinks(file);
		const fs::path directory = target.parent_path();
		if (!directory.empty())
			fs::create_directories(directory);

		// A name of its own, so that two processes writing the same file don't write into each other's copy.
		std::random_device random;
		const uint64_t suffix = (uint64_t{random()} << 32U) ^ random();
		char hex[g_temporaryDigits + 1] = {};
		std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(suffix));
		fs::path temporary = target;
		temporary += std::string(".") + hex + std::string(g_temporaryExtension);
		try
		{
			writeDurably(temporary, contents);
			fs::rename(temporary, target); // replaces the file in one step
		}
		catch (...)
		{
			std::error_code ignored;
			fs::remove(temporary, ignored);
			throw;
		}
		syncDirectory(directory);
	}

	void removeStaleTemporaries(const fs::path& file)
	{
		rt::assertNotRealtime("files::removeStaleTemporaries");
		const fs::path target = resolveLinks(file);
		const std::string name = target.filename().u8string();
		const fs::path directory = target.parent_path().empty() ? fs::path(".") : target.parent_path();
		std::error_code error;
		std::vector<fs::path> stale;
		for (fs::directory_iterator entry(directory, error), end; !error && entry != end; entry.increment(error))
		{
			if (isTemporaryOf(entry->path().filename().u8string(), name))
				stale.push_back(entry->path());
		}
		for (const fs::path& temporary : stale)
			fs::remove(temporary, error);
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

	FileLock::FileLock(const fs::path& file, std::chrono::milliseconds timeout)
	{
		rt::assertNotRealtime("files::FileLock");
		std::error_code ignored;
		if (!file.parent_path().empty())
			fs::create_directories(file.parent_path(), ignored); // opening the file reports what went wrong
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		constexpr std::chrono::milliseconds retry{10};
#ifdef _WIN32
		HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			throwLockError(static_cast<int>(GetLastError()), std::system_category(), file);
		for (;;)
		{
			OVERLAPPED overlapped = {};
			if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) != FALSE)
				break;
			const DWORD error = GetLastError();
			if (error != ERROR_LOCK_VIOLATION || std::chrono::steady_clock::now() >= deadline)
			{
				CloseHandle(handle);
				throwLockError(static_cast<int>(error), std::system_category(), file);
			}
			std::this_thread::sleep_for(retry);
		}
		m_handle = reinterpret_cast<intptr_t>(handle); // NOLINT(performance-no-int-to-ptr): an opaque handle
#else
		const int descriptor = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
		if (descriptor < 0)
			throwLockError(errno, std::generic_category(), file);
		// flock, not fcntl: its locks belong to the open file, so two locks in one process exclude each other too.
		while (flock(descriptor, LOCK_EX | LOCK_NB) != 0)
		{
			const int error = errno;
			if ((error != EWOULDBLOCK && error != EINTR) || std::chrono::steady_clock::now() >= deadline)
			{
				close(descriptor);
				throwLockError(error, std::generic_category(), file);
			}
			std::this_thread::sleep_for(retry);
		}
		m_handle = descriptor;
#endif
	}

	FileLock::~FileLock()
	{
#ifdef _WIN32
		CloseHandle(reinterpret_cast<HANDLE>(m_handle)); // NOLINT(performance-no-int-to-ptr): the handle it was
#else
		close(static_cast<int>(m_handle)); // releases the lock
#endif
	}
}
